#include "layouttree.h"

#include <QJsonValue>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>

#include <algorithm>

namespace layout {

namespace {

// A split never starves a child below this share, so a wild drag cannot
// leave a window at zero width and unrecoverable by mouse.
constexpr double kMinRatio = 0.05;
// Below this, a box is too small to subdivide meaningfully; splitting it
// evenly beats bounding it into a negative-width child.
constexpr int kMinSide = 40;
// A drag has to move an edge by more than this to count. Without it every
// re-tile's own rounding would rewrite ratios.
constexpr int kEdgeSlack = 2;

int rightOf(const QRect &r) { return r.x() + r.width(); }
int bottomOf(const QRect &r) { return r.y() + r.height(); }

bool overlaps(int aStart, int aLen, int bStart, int bLen)
{
    return aStart < bStart + bLen && bStart < aStart + aLen;
}

// The one line that defines dwindle: divide along the longer side, so each
// new window halves the squarest available box instead of shaving strips.
SplitKind splitKindFor(Policy policy, const QRect &box)
{
    switch (policy) {
    case Policy::Dwindle:
        break;
    }
    // An unarranged leaf (the very first split) has no box yet; columns is
    // what a wide screen wants.
    if (!box.isValid())
        return SplitKind::Columns;
    return box.width() >= box.height() ? SplitKind::Columns : SplitKind::Rows;
}

} // namespace

Tree::Tree() = default;
Tree::~Tree() = default;

bool Tree::fits(const QRect &box, SplitKind kind, const Metrics &metrics)
{
    // Each half is box/2 before gaps; the visible tile then loses a full gap,
    // half off each side.
    if (kind == SplitKind::Columns)
        return metrics.minWidth <= 0 || (box.width() / 2) - metrics.gap >= metrics.minWidth;
    return metrics.minHeight <= 0 || (box.height() / 2) - metrics.gap >= metrics.minHeight;
}

bool Tree::chooseSplit(const QRect &box, const Metrics &metrics, SplitKind *out) const
{
    const SplitKind natural = splitKindFor(m_policy, box);
    if (fits(box, natural, metrics)) {
        *out = natural;
        return true;
    }
    // A wide, short tile splits into rows perfectly well when columns would
    // leave a sliver. Rotating the split keeps the window tiled, which beats
    // ejecting it to the floating layer.
    const SplitKind other = (natural == SplitKind::Columns) ? SplitKind::Rows
                                                            : SplitKind::Columns;
    if (fits(box, other, metrics)) {
        *out = other;
        return true;
    }
    return false;
}

Insert Tree::insert(quintptr id, quintptr nearId, const Metrics &metrics)
{
    if (id == 0 || m_index.contains(id))
        return Insert::Placed;

    if (!m_root) {
        // The first window gets the whole work area; there is no split to be
        // too small, so the minimums cannot reject it.
        m_root = std::make_unique<Node>();
        m_root->id = id;
        m_index.insert(id, m_root.get());
        m_lastInserted = id;
        return Insert::Placed;
    }

    // The split axis is read off the target's box below, so the boxes have to
    // be current. Only arrange() writes them, and a batch adoption - enabling
    // the tiler, or a virtual desktop switch - runs every insert before the
    // first arrange. Without this the whole batch splits off an empty box and
    // comes out as one column per window.
    refreshBoxes(metrics);

    Node *target = m_index.value(nearId);
    if (!target)
        target = m_index.value(m_lastInserted);
    if (!target)
        target = *m_index.begin(); // the index holds nothing but leaves

    // Decided before anything is mutated, so a rejection leaves the tree
    // exactly as it was and the caller can float the window instead.
    SplitKind kind;
    if (!chooseSplit(target->box, metrics, &kind)) {
        // The preferred leaf has no room left. Rather than give up on the
        // whole insert, try every other leaf and take the one with the most
        // room - a batch adoption (nearId == 0, enabling the tiler or a
        // virtual-desktop switch with several windows already open) always
        // targets m_lastInserted, so without this the first window's leaf
        // keeps halving itself into slivers while its neighbours sit at
        // twice its size. m_index is a QHash, so its order is random per
        // process; largest area first, ties broken by top edge then left
        // edge, is what makes the pick reproducible.
        Node *best = nullptr;
        SplitKind bestKind = SplitKind::Columns;
        qint64 bestArea = -1;
        for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
            Node *leaf = it.value();
            SplitKind candidateKind;
            if (!chooseSplit(leaf->box, metrics, &candidateKind))
                continue;
            const qint64 area = qint64(leaf->box.width()) * leaf->box.height();
            const bool better = !best || area > bestArea
                              || (area == bestArea && leaf->box.top() < best->box.top())
                              || (area == bestArea && leaf->box.top() == best->box.top()
                                  && leaf->box.left() < best->box.left());
            if (better) {
                best = leaf;
                bestKind = candidateKind;
                bestArea = area;
            }
        }
        if (!best)
            return Insert::TooSmall;
        target = best;
        kind = bestKind;
    }

    // The target leaf becomes the split in place, so no parent pointer
    // anywhere else has to be rewritten.
    const quintptr moved = target->id;

    auto first = std::make_unique<Node>();
    first->id = moved;
    first->parent = target;
    auto second = std::make_unique<Node>();
    second->id = id;
    second->parent = target;

    target->id = 0;
    target->kind = kind;
    target->ratio = 0.5;
    target->a = std::move(first);
    target->b = std::move(second);

    m_index[moved] = target->a.get();
    m_index.insert(id, target->b.get());
    m_lastInserted = id;
    return Insert::Placed;
}

bool Tree::remove(quintptr id)
{
    Node *node = m_index.value(id);
    if (!node)
        return false;
    m_index.remove(id);
    if (m_lastInserted == id)
        m_lastInserted = 0;

    Node *parent = node->parent;
    if (!parent) {
        m_root.reset();
        return true;
    }

    // Lift the sibling into the parent's slot. Assigning over that slot
    // destroys the parent, and with it the branch still holding `node`.
    std::unique_ptr<Node> sibling =
        (parent->a.get() == node) ? std::move(parent->b) : std::move(parent->a);
    Node *grand = parent->parent;
    sibling->parent = grand;

    std::unique_ptr<Node> &slot =
        grand ? (grand->a.get() == parent ? grand->a : grand->b) : m_root;
    slot = std::move(sibling);
    return true;
}

QVector<quintptr> Tree::ids() const
{
    QVector<Placement> places;
    collect(m_root.get(), 0, places);
    QVector<quintptr> out;
    out.reserve(places.size());
    for (const Placement &p : places)
        out.append(p.id);
    return out;
}

void Tree::collect(Node *node, int half, QVector<Placement> &out) const
{
    if (!node)
        return;
    if (node->isLeaf()) {
        out.append({ node->id, node->box.adjusted(half, half, -half, -half) });
        return;
    }
    collect(node->a.get(), half, out);
    collect(node->b.get(), half, out);
}

void Tree::computeBoxes(Node *node, const QRect &box, const Metrics &metrics)
{
    if (!node)
        return;
    node->box = box;
    if (node->isLeaf())
        return;

    // Every rect the tiler ever applies comes through here - insertion, a
    // drag, a resolution change, a gap edit - so this is where the minimum is
    // enforced. Guarding insert() alone is not enough: a ratio far from 0.5
    // shrinks a pane without any leaf being added. A box with no room for two
    // floors splits evenly, because no arrangement of it can satisfy them.
    if (node->kind == SplitKind::Columns) {
        const int floorPx = qMax(kMinSide, metrics.minWidth + metrics.gap);
        int w = qRound(box.width() * node->ratio);
        if (box.width() >= 2 * floorPx)
            w = qBound(floorPx, w, box.width() - floorPx);
        computeBoxes(node->a.get(), QRect(box.x(), box.y(), w, box.height()), metrics);
        computeBoxes(node->b.get(),
                     QRect(box.x() + w, box.y(), box.width() - w, box.height()), metrics);
    } else {
        const int floorPx = qMax(kMinSide, metrics.minHeight + metrics.gap);
        int h = qRound(box.height() * node->ratio);
        if (box.height() >= 2 * floorPx)
            h = qBound(floorPx, h, box.height() - floorPx);
        computeBoxes(node->a.get(), QRect(box.x(), box.y(), box.width(), h), metrics);
        computeBoxes(node->b.get(),
                     QRect(box.x(), box.y() + h, box.width(), box.height() - h), metrics);
    }
}

void Tree::refreshBoxes(const Metrics &metrics)
{
    // Partition a box grown by half a gap, then shrink every leaf by that
    // same half: neighbours end up `gap` apart and the outermost edges sit
    // `outerGap` from the work area. Deliberately allowed to go negative -
    // outerGap 0 with gap 16 means flush to the screen, 16 between windows.
    const int inset = metrics.outerGap - metrics.gap / 2;
    computeBoxes(m_root.get(), metrics.area.adjusted(inset, inset, -inset, -inset), metrics);
}

QVector<Placement> Tree::arrange(const Metrics &metrics)
{
    QVector<Placement> out;
    if (!m_root)
        return out;
    refreshBoxes(metrics);
    out.reserve(m_index.size());
    collect(m_root.get(), metrics.gap / 2, out);
    return out;
}

Tree::Node *Tree::ownerOf(Node *leaf, Edge edge)
{
    // The divider between `a` and `b` is `a`'s far edge and `b`'s near edge,
    // so the ancestor owning this edge is the first one that splits on the
    // matching axis with our subtree on the matching side.
    const bool wantColumns = (edge == Edge::Left || edge == Edge::Right);
    const bool wantFirstChild = (edge == Edge::Right || edge == Edge::Bottom);

    for (Node *n = leaf; n->parent; n = n->parent) {
        Node *p = n->parent;
        if ((p->kind == SplitKind::Columns) != wantColumns)
            continue;
        if ((p->a.get() == n) == wantFirstChild)
            return p;
    }
    return nullptr;
}

void Tree::applyResize(quintptr id, const QRect &actual, const Metrics &metrics)
{
    Node *leaf = m_index.value(id);
    if (!leaf || !leaf->parent)
        return;

    refreshBoxes(metrics);

    // The dragged rect carries the gap inset; the dividers live in the
    // un-gapped partition, so put the half-gap back before comparing.
    const int half = metrics.gap / 2;
    const QRect want(actual.x() - half, actual.y() - half,
                     actual.width() + metrics.gap, actual.height() + metrics.gap);
    const QRect before = leaf->box;

    // Captured up front: every ratio written below moves the boxes, so the
    // "did this edge move" question has to be asked before any of them.
    const struct { Edge edge; bool moved; } edges[] = {
        { Edge::Left,   qAbs(want.x() - before.x()) > kEdgeSlack },
        { Edge::Right,  qAbs(rightOf(want) - rightOf(before)) > kEdgeSlack },
        { Edge::Top,    qAbs(want.y() - before.y()) > kEdgeSlack },
        { Edge::Bottom, qAbs(bottomOf(want) - bottomOf(before)) > kEdgeSlack },
    };

    for (const auto &e : edges) {
        if (!e.moved)
            continue;
        Node *owner = ownerOf(leaf, e.edge);
        if (!owner)
            continue;

        const QRect &box = owner->box;
        double ratio = owner->ratio;
        if (e.edge == Edge::Left || e.edge == Edge::Right) {
            if (box.width() <= 0)
                continue;
            const int divider = (e.edge == Edge::Left) ? want.x() : rightOf(want);
            ratio = double(divider - box.x()) / box.width();
        } else {
            if (box.height() <= 0)
                continue;
            const int divider = (e.edge == Edge::Top) ? want.y() : bottomOf(want);
            ratio = double(divider - box.y()) / box.height();
        }

        owner->ratio = qBound(kMinRatio, ratio, 1.0 - kMinRatio);
        refreshBoxes(metrics); // the next edge reads fresh boxes
    }
}

bool Tree::resize(quintptr id, SplitKind axis, int delta, const Metrics &metrics)
{
    Node *leaf = m_index.value(id);
    if (!leaf || !leaf->parent || delta == 0)
        return false;

    refreshBoxes(metrics);

    const bool columns = (axis == SplitKind::Columns);
    // The nearest ancestor splitting this axis. A higher one would move too:
    // its divider is the edge of a whole column, so paying this tile its
    // pixels would widen its neighbours by the same amount. Everything
    // between here and the nearest one splits the other way, which is what
    // makes the tile itself take the whole delta.
    Node *owner = nullptr;
    int sign = 1;
    for (Node *n = leaf; n->parent; n = n->parent) {
        if ((n->parent->kind == SplitKind::Columns) != columns)
            continue;
        owner = n->parent;
        // Before the divider the tile grows by pushing it out, after it by
        // pulling it back.
        sign = (owner->a.get() == n) ? 1 : -1;
        break;
    }
    if (!owner)
        return false; // nothing splits this axis: no neighbour to take from

    const int span = columns ? owner->box.width() : owner->box.height();
    if (span <= 0)
        return false;

    // Read the divider off the rendered box rather than off the ratio: the
    // minimum-size clamp in computeBoxes may already have moved it, and a
    // step measured from where the divider actually is keeps grow and shrink
    // symmetric.
    const QRect &aBox = owner->a->box;
    const int divider = columns ? aBox.width() : aBox.height();
    const double ratio = qBound(kMinRatio, double(divider + sign * delta) / span,
                                1.0 - kMinRatio);
    if (qFuzzyCompare(ratio, owner->ratio))
        return false;

    owner->ratio = ratio;
    refreshBoxes(metrics);
    return true;
}

quintptr Tree::neighbour(quintptr id, Direction dir) const
{
    Node *self = m_index.value(id);
    if (!self)
        return 0;
    const QRect me = self->box;
    const QPoint c = me.center();

    quintptr best = 0;
    int bestDistance = 0;
    int bestCross = 0;
    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
        if (it.key() == id)
            continue;
        const QRect r = it.value()->box;
        const QPoint o = r.center();

        bool onSide = false;
        bool inLane = false;
        int distance = 0;
        int cross = 0; // offset along the other axis, the tie-break
        switch (dir) {
        case Direction::Left:
            onSide = o.x() < c.x();
            inLane = overlaps(me.y(), me.height(), r.y(), r.height());
            distance = c.x() - o.x();
            cross = qAbs(o.y() - c.y());
            break;
        case Direction::Right:
            onSide = o.x() > c.x();
            inLane = overlaps(me.y(), me.height(), r.y(), r.height());
            distance = o.x() - c.x();
            cross = qAbs(o.y() - c.y());
            break;
        case Direction::Up:
            onSide = o.y() < c.y();
            inLane = overlaps(me.x(), me.width(), r.x(), r.width());
            distance = c.y() - o.y();
            cross = qAbs(o.x() - c.x());
            break;
        case Direction::Down:
            onSide = o.y() > c.y();
            inLane = overlaps(me.x(), me.width(), r.x(), r.width());
            distance = o.y() - c.y();
            cross = qAbs(o.x() - c.x());
            break;
        }
        if (!onSide || !inLane)
            continue;
        // A tall pane beside a stack of short ones puts several candidates
        // at the same distance. Without the cross tie-break the winner comes
        // out of QHash order, i.e. the same chord lands somewhere different
        // from one run to the next.
        if (!best || distance < bestDistance
            || (distance == bestDistance && cross < bestCross)) {
            best = it.key();
            bestDistance = distance;
            bestCross = cross;
        }
    }
    return best;
}

bool Tree::swap(quintptr a, quintptr b)
{
    Node *na = m_index.value(a);
    Node *nb = m_index.value(b);
    if (!na || !nb || na == nb)
        return false;
    // Only the payloads move: keeping the partition and exchanging its
    // occupants is exactly what "move window left" should feel like.
    std::swap(na->id, nb->id);
    m_index[a] = nb;
    m_index[b] = na;
    return true;
}

void Tree::resetRatios(Node *node)
{
    if (!node || node->isLeaf())
        return;
    node->ratio = 0.5;
    resetRatios(node->a.get());
    resetRatios(node->b.get());
}

void Tree::equalize()
{
    resetRatios(m_root.get());
}

bool Tree::toggleSplit(quintptr id)
{
    Node *leaf = m_index.value(id);
    if (!leaf || !leaf->parent)
        return false;
    Node *p = leaf->parent;
    p->kind = (p->kind == SplitKind::Columns) ? SplitKind::Rows : SplitKind::Columns;
    return true;
}

// --------------------------------------------------------------- persistence

QJsonObject Tree::nodeToJson(const Node *node)
{
    if (!node)
        return QJsonObject(); // never reached from toJson(): a and b always exist together
    if (node->isLeaf())
        return QJsonObject{ { QStringLiteral("id"), double(node->id) } };
    return QJsonObject{
        { QStringLiteral("kind"), node->kind == SplitKind::Columns
              ? QStringLiteral("columns") : QStringLiteral("rows") },
        { QStringLiteral("ratio"), node->ratio },
        { QStringLiteral("a"), nodeToJson(node->a.get()) },
        { QStringLiteral("b"), nodeToJson(node->b.get()) },
    };
}

QJsonObject Tree::toJson() const
{
    return nodeToJson(m_root.get());
}

std::unique_ptr<Tree::Node> Tree::nodeFromJson(const QJsonObject &obj, Node *parent,
                                               const std::function<bool(quintptr)> &accept,
                                               QHash<quintptr, Node *> *index, bool *malformed)
{
    if (obj.contains(QStringLiteral("id"))) {
        const QJsonValue idVal = obj.value(QStringLiteral("id"));
        if (!idVal.isDouble()) {
            *malformed = true;
            return nullptr;
        }
        const quintptr id = quintptr(idVal.toDouble());
        // 0 means "no target" to insert(); a duplicate would corrupt m_index.
        if (id == 0 || index->contains(id)) {
            *malformed = true;
            return nullptr;
        }
        if (accept && !accept(id))
            return nullptr; // rejected, not malformed - caller collapses around the gap

        auto leaf = std::make_unique<Node>();
        leaf->id = id;
        leaf->parent = parent;
        index->insert(id, leaf.get());
        return leaf;
    }

    const QJsonValue kindVal = obj.value(QStringLiteral("kind"));
    const QJsonValue ratioVal = obj.value(QStringLiteral("ratio"));
    const QJsonValue aVal = obj.value(QStringLiteral("a"));
    const QJsonValue bVal = obj.value(QStringLiteral("b"));
    if (!kindVal.isString() || !ratioVal.isDouble() || !aVal.isObject() || !bVal.isObject()) {
        *malformed = true;
        return nullptr;
    }
    SplitKind kind;
    if (kindVal.toString() == QLatin1String("columns"))
        kind = SplitKind::Columns;
    else if (kindVal.toString() == QLatin1String("rows"))
        kind = SplitKind::Rows;
    else {
        *malformed = true;
        return nullptr;
    }

    auto split = std::make_unique<Node>();
    split->parent = parent;
    split->kind = kind;
    split->ratio = qBound(kMinRatio, ratioVal.toDouble(), 1.0 - kMinRatio);

    std::unique_ptr<Node> a = nodeFromJson(aVal.toObject(), split.get(), accept, index, malformed);
    if (*malformed)
        return nullptr;
    std::unique_ptr<Node> b = nodeFromJson(bVal.toObject(), split.get(), accept, index, malformed);
    if (*malformed)
        return nullptr;

    // A lone survivor takes the split's place, exactly like remove() lifting
    // the sibling; neither means the split collapses with them.
    if (a && b) {
        split->a = std::move(a);
        split->b = std::move(b);
        return split;
    }
    if (a) {
        a->parent = parent;
        return a;
    }
    if (b) {
        b->parent = parent;
        return b;
    }
    return nullptr;
}

bool Tree::fromJson(const QJsonObject &obj, const std::function<bool(quintptr)> &accept)
{
    // {} is toJson()'s empty tree, not malformed: the caller's only answer to
    // malformed is to distrust the whole state file.
    if (obj.isEmpty()) {
        m_root.reset();
        m_index.clear();
        m_lastInserted = 0;
        return true;
    }

    QHash<quintptr, Node *> index;
    bool malformed = false;
    std::unique_ptr<Node> root = nodeFromJson(obj, nullptr, accept, &index, &malformed);
    if (malformed)
        return false; // tree left untouched

    m_root = std::move(root); // may be null: every id rejected is a valid empty tree
    m_index = std::move(index);
    m_lastInserted = 0;
    return true;
}

} // namespace layout
