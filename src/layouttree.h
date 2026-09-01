#pragma once

#include <QHash>
#include <QRect>
#include <QVector>

#include <memory>

// The layout half of the tiler: a binary space partition over opaque ids,
// with no Win32 and no notion of a window. `tilingapi.*` owns the native
// half and passes HWNDs in as ids.
//
// A node is either a leaf carrying an id or a split carrying a kind, a ratio
// and two children. Rects are always re-derived from the tree, never stored
// as state - the split ratios are the only thing that survives, which is
// what makes a resized layout hold its shape across re-tiles.
//
// The insertion policy is what makes a named layout. `Dwindle` (Hyprland's
// default) splits the target leaf along its longer side, which is what stops
// the halves degenerating into slivers. Another layout is another Policy
// value and another branch in splitKindFor(); the tree, the resize maths and
// the navigation are all policy-independent.
namespace layout {

// Columns puts `a` left of `b` (a vertical divider); Rows puts `a` above
// `b`. Named for the arrangement, not the divider: "horizontal split" means
// opposite things in i3 and in Qt, and we read this code next to both.
enum class SplitKind { Columns, Rows };

enum class Direction { Left, Right, Up, Down };

enum class Policy { Dwindle };

struct Placement {
    quintptr id = 0;
    QRect rect;
};

// Everything needed to turn the tree into pixels, passed rather than stored:
// the tree holds ratios, never geometry, so the same tree lays out correctly
// on a monitor that changed resolution or scale under it.
//
// `minWidth` / `minHeight` are the smallest *visible* tile the layout will
// ever produce; 0 disables the check. Enforced in computeBoxes, so it binds
// every path equally - a new window, a dragged divider, a resolution change -
// not just insertion. kMinSide in the .cpp is the absolute floor underneath it.
struct Metrics {
    QRect area;
    int gap = 0;
    int outerGap = 0;
    int minWidth = 0;
    int minHeight = 0;
};

// TooSmall: both split axes would have left a tile under the minimum, so
// nothing was inserted and the tree is untouched. The caller floats it.
enum class Insert { Placed, TooSmall };

class Tree
{
public:
    Tree();
    ~Tree();
    Tree(const Tree &) = delete;
    Tree &operator=(const Tree &) = delete;

    void setPolicy(Policy policy) { m_policy = policy; }
    Policy policy() const { return m_policy; }

    bool isEmpty() const { return m_index.isEmpty(); }
    int count() const { return m_index.size(); }
    bool contains(quintptr id) const { return m_index.contains(id); }
    QVector<quintptr> ids() const; // leaves, left-to-right / top-to-bottom

    // `nearId` is the leaf to split - normally the focused window, which is
    // what makes dwindle grow where the user is looking. An id that is not
    // in the tree falls back to the most recent insertion. Spelt out rather
    // than `near`, which windows.h still defines as an empty macro.
    //
    // Metrics is an argument because the split axis is read off the target
    // leaf's current box: adopting a batch of windows runs many inserts
    // between two arrange() calls, so insert() has to recompute rather than
    // trust what the last arrange left behind.
    //
    // If the natural axis would leave a tile under the minimum, the other
    // axis is tried before giving up - a wide, short tile often splits into
    // rows perfectly well when columns would not, and rotating the split
    // beats ejecting the window.
    Insert insert(quintptr id, quintptr nearId, const Metrics &metrics);
    bool remove(quintptr id);

    // Rects for every leaf: `gap` between neighbours, `outerGap` from the
    // edge of the area. Also refreshes the box cache that applyResize() and
    // neighbour() read, so it is the natural thing to call first.
    QVector<Placement> arrange(const Metrics &metrics);

    // The user dragged `id` to `actual`. Every edge that moved is traced to
    // the ancestor split that owns it and that split's ratio is rewritten,
    // so everything on the far side of the divider follows and the partition
    // stays a partition. An edge with no owning ancestor is a screen edge
    // and is ignored - there is nothing on the other side to give space to.
    void applyResize(quintptr id, const QRect &actual, const Metrics &metrics);

    // Keyboard resize: `delta` pixels wider (Columns) or taller (Rows), and
    // narrower / shorter when negative. The divider that moves is the nearest
    // ancestor splitting this axis, so the tile takes the whole delta and its
    // neighbours on the other side of a higher divider are left alone. False
    // when no ancestor splits this axis, or the split was already at its limit.
    bool resize(quintptr id, SplitKind axis, int delta, const Metrics &metrics);

    // Nearest leaf in `dir` that overlaps on the perpendicular axis, from
    // the last arrange(). Geometric rather than tree-walking: in a deep
    // dwindle the tree's idea of "left" is frequently not the screen's.
    quintptr neighbour(quintptr id, Direction dir) const;

    bool swap(quintptr a, quintptr b);
    // Flips the split that placed `id` - the one-key fix for a dwindle that
    // divided the wrong way.
    bool toggleSplit(quintptr id);
    // Every split back to a half, discarding the resize history.
    void equalize();

private:
    struct Node {
        quintptr id = 0;                      // leaf only
        SplitKind kind = SplitKind::Columns;  // split only
        double ratio = 0.5;                   // split only: `a`'s share
        Node *parent = nullptr;
        std::unique_ptr<Node> a, b;
        QRect box;                            // last arrange(), before gaps

        bool isLeaf() const { return !a; }
    };

    enum class Edge { Left, Right, Top, Bottom };

    static void resetRatios(Node *node);
    // Would halving this box along `kind` leave a visible tile at or above
    // the minimum? Both halves are equal at ratio 0.5, so one test covers them.
    static bool fits(const QRect &box, SplitKind kind, const Metrics &metrics);
    bool chooseSplit(const QRect &box, const Metrics &metrics, SplitKind *out) const;
    void computeBoxes(Node *node, const QRect &box, const Metrics &metrics);
    void refreshBoxes(const Metrics &metrics);
    // First ancestor whose divider lies on `edge` of this leaf's subtree.
    static Node *ownerOf(Node *leaf, Edge edge);
    void collect(Node *node, int half, QVector<Placement> &out) const;

    std::unique_ptr<Node> m_root;
    QHash<quintptr, Node *> m_index;
    Policy m_policy = Policy::Dwindle;
    quintptr m_lastInserted = 0;
};

} // namespace layout
