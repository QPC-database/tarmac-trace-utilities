/*
 * Copyright 2016-2021 Arm Limited. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is part of Tarmac Trace Utilities
 */

#ifndef LIBTARMAC_DISKTREE_HH
#define LIBTARMAC_DISKTREE_HH

#include <cassert>
#include <functional>
#include <string>

class MMapFile {
    struct PlatformData;

    const std::string filename;
    bool writable;
    PlatformData *pdata;
    off_t curr_size, next_offset;
    void *mapping;

    void map();
    void unmap();

  public:
    MMapFile(const std::string &filename, bool writable);
    ~MMapFile();

    off_t alloc(size_t size);
    off_t curr_offset() const { return next_offset; }

    template <class T> inline T *getptr(off_t offset)
    {
        assert(0 <= offset && (off_t)sizeof(T) <= next_offset &&
               offset <= next_offset - (off_t)sizeof(T));
        return (T *)((char *)mapping + offset);
    }

    template <class T> inline const T *getptr(off_t offset) const
    {
        assert(0 <= offset && (off_t)sizeof(T) <= next_offset &&
               offset <= next_offset - (off_t)sizeof(T));
        return (const T *)((char *)mapping + offset);
    }

    template <class T> inline T *newptr()
    {
        return getptr<T>(alloc(sizeof(T)));
    }
};

template <class Int> class diskint {
    unsigned char bytes[sizeof(Int)];
    inline void set(Int val)
    {
        for (size_t i = 0; i < sizeof(bytes); i++) {
            bytes[sizeof(bytes) - 1 - i] = val;
            val >>= 8;
        }
    }

  public:
    diskint() { set(0); }
    diskint(Int val) { set(val); }
    void operator=(Int val) { set(val); }
    // This function is redundant given the operator wrapper below,
    // but useful for gdb to set breakpoints on, because its syntax
    // has trouble specifying 'operator unsigned long long' and the
    // like, for some reason.
    Int value() const
    {
        Int ret = 0;
        for (size_t i = 0; i < sizeof(bytes); i++) {
            ret = (ret << 8) + bytes[i];
        }
        return ret;
    }
    inline operator Int() const { return value(); }
};

enum class WalkOrder { Preorder, Inorder, Postorder };

template <class Payload> class EmptyAnnotation {
  public:
    EmptyAnnotation() {}
    EmptyAnnotation(const Payload &) {}
    EmptyAnnotation(const EmptyAnnotation &, const EmptyAnnotation &) {}
};

template <class Payload, class Annotation = EmptyAnnotation<Payload>>
class AVLDisk {
    MMapFile &mmf;
    // High-water mark. Nodes at addresses below here are already used
    // by some prior tree root and hence are immutable.
    off_t hwm;

    struct node {
        // offset can be 0, meaning this is the null node
        off_t offset, lc, rc;
        int height;
        Payload payload;
        Annotation annotation;
    };

    struct disknode {
        diskint<off_t> lc, rc;
        diskint<int> height;
        Payload payload;
        Annotation annotation;
    };

    void put(node &n)
    {
        disknode &dn = *mmf.getptr<disknode>(n.offset);
        dn.lc = n.lc;
        dn.rc = n.rc;
        dn.height = n.height;
        dn.payload = n.payload;
        dn.annotation = n.annotation;
    }

    node get(off_t offset) const
    {
        node n;
        if (offset == 0) {
            n.offset = 0;
            n.lc = n.rc = 0;
            n.height = 0;
        } else {
            disknode &dn = *mmf.getptr<disknode>(offset);
            n.offset = offset;
            n.lc = dn.lc;
            n.rc = dn.rc;
            n.height = dn.height;
            n.payload = dn.payload;
            n.annotation = dn.annotation;
        }
        return n;
    }

    bool immutable(node &n) const { return n.offset < hwm; }

    void rewrite(node &n, off_t newlc, off_t newrc)
    {
        if (immutable(n))
            n.offset = mmf.alloc(sizeof(disknode));

        n.lc = newlc;
        n.rc = newrc;
        n.height = std::max(get(newlc).height, get(newrc).height) + 1;
        n.annotation = Annotation(n.payload);
        if (n.lc) {
            node lc = get(n.lc);
            n.annotation = Annotation(lc.annotation, n.annotation);
        }
        if (n.rc) {
            node rc = get(n.rc);
            n.annotation = Annotation(n.annotation, rc.annotation);
        }
        put(n);
    }

    node rotate_left(node &n)
    {
        node rc = get(n.rc);
        off_t t0 = n.lc, t1 = rc.lc, t2 = rc.rc;
        rewrite(n, t0, t1);
        rewrite(rc, n.offset, t2);
        return rc;
    }

    node rotate_right(node &n)
    {
        node lc = get(n.lc);
        off_t t0 = lc.lc, t1 = lc.rc, t2 = n.rc;
        rewrite(n, t1, t2);
        rewrite(lc, t0, n.offset);
        return lc;
    }

    node insert_main(node &root, node &n)
    {
        if (root.offset == 0)
            return n;

        node lc = get(root.lc), rc = get(root.rc);
        int k;

        int cmp = root.payload.cmp(n.payload);
        assert(cmp != 0);

        if (cmp > 0) {
            lc = insert_main(lc, n);
            rewrite(root, lc.offset, rc.offset);
            k = rc.height;

            if (lc.height == k + 2) {
                node lrc = get(lc.rc);
                if (lrc.height == k + 1) {
                    lc = rotate_left(lc);
                    rewrite(root, lc.offset, rc.offset);
                }
                return rotate_right(root);
            }
        } else {
            rc = insert_main(rc, n);
            rewrite(root, lc.offset, rc.offset);
            k = lc.height;

            if (rc.height == k + 2) {
                node rlc = get(rc.lc);
                if (rlc.height == k + 1) {
                    rc = rotate_right(rc);
                    rewrite(root, lc.offset, rc.offset);
                }
                return rotate_left(root);
            }
        }

        return root;
    }

    template <class PayloadComparable>
    node remove_main(node &root, const PayloadComparable *keyfinder,
                     node *removed)
    {

        if (root.offset == 0) {
            // element to be removed was not found
            *removed = root;
            return root;
        }
        node lc = get(root.lc), rc = get(root.rc);
        int k;

        int cmp;
        if (keyfinder) {
            cmp = keyfinder->cmp(root.payload);
        } else {
            cmp = root.lc ? -1 : 0;
        }

        if (cmp < 0) {
            off_t oldlc = lc.offset;
            lc = remove_main(lc, keyfinder, removed);
            if (lc.offset == oldlc)
                return root;

            rewrite(root, lc.offset, rc.offset);
            k = lc.height;

            if (rc.height == k + 2) {
                node rlc = get(rc.lc);
                if (rlc.height == k + 1) {
                    rc = rotate_right(rc);
                    rewrite(root, lc.offset, rc.offset);
                }
                return rotate_left(root);
            }
        } else {
            if (cmp > 0) {
                off_t oldrc = rc.offset;
                rc = remove_main(rc, keyfinder, removed);
                if (rc.offset == oldrc)
                    return root;
            } else {
                *removed = root;
                if (!root.lc && !root.rc) {
                    return get(0);
                } else if (!root.lc) {
                    return get(root.rc);
                } else if (!root.rc) {
                    return get(root.lc);
                } else {
                    rc = remove_main<PayloadComparable>(rc, nullptr, &root);
                    rewrite(root, lc.offset, rc.offset);
                }
            }

            rewrite(root, lc.offset, rc.offset);
            k = rc.height;

            if (lc.height == k + 2) {
                node lrc = get(lc.rc);
                if (lrc.height == k + 1) {
                    lc = rotate_left(lc);
                    rewrite(root, lc.offset, rc.offset);
                }
                return rotate_right(root);
            }
        }

        return root;
    }

    template <class PayloadComparable>
    bool find_main(node &root, const PayloadComparable *keyfinder,
                   node *found) const
    {
        if (root.offset == 0)
            return false;

        int cmp = keyfinder->cmp(root.payload);

        if (cmp == 0) {
            *found = root;
            return true;
        } else if (cmp < 0) {
            node child = get(root.lc);
            return find_main(child, keyfinder, found);
        } else {
            node child = get(root.rc);
            return find_main(child, keyfinder, found);
        }
    }

    template <class PayloadComparable>
    bool find_leftmost_main(node &root, const PayloadComparable *keyfinder,
                            node *found) const
    {
        if (root.offset == 0)
            return false;

        int cmp = keyfinder->cmp(root.payload);

        if (cmp == 0) {
            node child = get(root.lc);
            if (!find_leftmost_main(child, keyfinder, found))
                *found = root;
            return true;
        } else if (cmp < 0) {
            node child = get(root.lc);
            return find_leftmost_main(child, keyfinder, found);
        } else {
            node child = get(root.rc);
            return find_leftmost_main(child, keyfinder, found);
        }
    }

    template <class PayloadComparable>
    bool find_rightmost_main(node &root, const PayloadComparable *keyfinder,
                             node *found) const
    {
        if (root.offset == 0)
            return false;

        int cmp = keyfinder->cmp(root.payload);

        if (cmp == 0) {
            node child = get(root.rc);
            if (!find_rightmost_main(child, keyfinder, found))
                *found = root;
            return true;
        } else if (cmp < 0) {
            node child = get(root.lc);
            return find_rightmost_main(child, keyfinder, found);
        } else {
            node child = get(root.rc);
            return find_rightmost_main(child, keyfinder, found);
        }
    }

    template <class PayloadComparable>
    bool predsucc_main(node &root, const PayloadComparable *keyfinder,
                       node *found, int sign) const
    {
        if (root.offset == 0)
            return false;

        int cmp = keyfinder->cmp(root.payload);
        if (cmp == 0) {
            // We've found the element whose successor/predecessor is
            // what we really want, so pretend it was just too
            // small/big (respectively).
            cmp = sign;
        }

        if (cmp < 0) {
            node child = get(root.lc);
            bool ret = predsucc_main(child, keyfinder, found, sign);
            if (sign > 0) {
                if (!ret)
                    *found = root;
                return true;
            } else {
                return ret;
            }
        } else {
            node child = get(root.rc);
            bool ret = predsucc_main(child, keyfinder, found, sign);
            if (sign < 0) {
                if (!ret)
                    *found = root;
                return true;
            } else {
                return ret;
            }
        }
    }

  public:
    AVLDisk(MMapFile &mmf) : mmf(mmf) { hwm = mmf.curr_offset(); }

    void commit() { hwm = mmf.curr_offset(); }

    template <class PayloadComparable>
    off_t remove(off_t oldroot, const PayloadComparable &keyfinder, bool *found,
                 Payload *removed_payload)
    {

        node root = get(oldroot);
        node removed;
        root = remove_main(root, &keyfinder, &removed);
        if (found)
            *found = removed.offset != 0;
        if (removed_payload && removed.offset != 0)
            *removed_payload = removed.payload;
        return root.offset;
    }

    template <class PayloadComparable>
    bool find(off_t root_offset, const PayloadComparable &keyfinder,
              Payload *payload_out, off_t *offset_out) const
    {

        node root = get(root_offset);
        node found;
        bool ret = find_main(root, &keyfinder, &found);
        if (ret) {
            if (payload_out)
                *payload_out = found.payload;
            if (offset_out)
                *offset_out = found.offset;
        }
        return ret;
    }

    template <class PayloadComparable>
    bool find_leftmost(off_t root_offset, const PayloadComparable &keyfinder,
                       Payload *payload_out, off_t *offset_out) const
    {

        node root = get(root_offset);
        node found;
        bool ret = find_leftmost_main(root, &keyfinder, &found);
        if (ret) {
            if (payload_out)
                *payload_out = found.payload;
            if (offset_out)
                *offset_out = found.offset;
        }
        return ret;
    }

    template <class PayloadComparable>
    bool find_rightmost(off_t root_offset, const PayloadComparable &keyfinder,
                        Payload *payload_out, off_t *offset_out) const
    {

        node root = get(root_offset);
        node found;
        bool ret = find_rightmost_main(root, &keyfinder, &found);
        if (ret) {
            if (payload_out)
                *payload_out = found.payload;
            if (offset_out)
                *offset_out = found.offset;
        }
        return ret;
    }

    template <class PayloadComparable>
    bool succ(off_t root_offset, const PayloadComparable &keyfinder,
              Payload *payload_out, off_t *offset_out) const
    {

        node root = get(root_offset);
        node found;
        bool ret = predsucc_main(root, &keyfinder, &found, +1);
        if (ret) {
            if (payload_out)
                *payload_out = found.payload;
            if (offset_out)
                *offset_out = found.offset;
        }
        return ret;
    }

    template <class PayloadComparable>
    bool pred(off_t root_offset, const PayloadComparable &keyfinder,
              Payload *payload_out, off_t *offset_out) const
    {

        node root = get(root_offset);
        node found;
        bool ret = predsucc_main(root, &keyfinder, &found, -1);
        if (ret) {
            if (payload_out)
                *payload_out = found.payload;
            if (offset_out)
                *offset_out = found.offset;
        }
        return ret;
    }

    off_t insert(off_t oldroot, Payload payload)
    {
        node root = get(oldroot);
        node n;
        n.offset = mmf.alloc(sizeof(disknode));
        n.lc = 0;
        n.rc = 0;
        n.height = 1;
        n.payload = payload;
        n.annotation = Annotation(n.payload);
        put(n);
        root = insert_main(root, n);
        return root.offset;
    }

    using Searcher =
        std::function<int(off_t, const Annotation *, off_t, const Payload &,
                          const Annotation &, off_t, const Annotation *)>;

    bool search(off_t nodeoff, Searcher searcher, Payload *payload_out)
    {
        disknode *node;
        Annotation *lca, *rca;

        while (nodeoff) {
            node = mmf.getptr<disknode>(nodeoff);
            lca = node->lc ? &mmf.getptr<disknode>(node->lc)->annotation
                           : nullptr;
            rca = node->rc ? &mmf.getptr<disknode>(node->rc)->annotation
                           : nullptr;
            int direction = searcher(node->lc, lca, nodeoff, node->payload,
                                     node->annotation, node->rc, rca);
            if (direction < 0) {
                nodeoff = node->lc;
            } else if (direction > 0) {
                nodeoff = node->rc;
            } else {
                if (payload_out)
                    *payload_out = node->payload;
                return true;
            }
        }
        return false;
    }

    using WalkVisitor =
        std::function<void(Payload &, Annotation &, off_t, Annotation *, off_t,
                           Annotation *, off_t)>;

    void walk(off_t nodeoff, WalkOrder order, WalkVisitor visitor)
    {
        node n, lc, rc;
        Annotation *lca, *rca;

        if (!nodeoff)
            return;

        n = get(nodeoff);

        if (n.lc && order != WalkOrder::Preorder)
            walk(n.lc, order, visitor);
        if (n.rc && order == WalkOrder::Postorder)
            walk(n.rc, order, visitor);

        lca = n.lc ? (lc = get(n.lc), &lc.annotation) : nullptr;
        rca = n.rc ? (rc = get(n.rc), &rc.annotation) : nullptr;
        visitor(n.payload, n.annotation, n.lc, lca, n.rc, rca, nodeoff);
        if (n.lc)
            put(lc);
        if (n.rc)
            put(rc);

        if (n.lc && order == WalkOrder::Preorder)
            walk(n.lc, order, visitor);
        if (n.rc && order != WalkOrder::Postorder)
            walk(n.rc, order, visitor);

        put(n);
    }

    using ConstWalkVisitor = std::function<void(
        const Payload &, const Annotation &, off_t, const Annotation *, off_t,
        const Annotation *, off_t)>;

    void walk(off_t nodeoff, WalkOrder order,
              const ConstWalkVisitor &visitor) const
    {
        node n, lc, rc;
        Annotation *lca, *rca;

        if (!nodeoff)
            return;

        n = get(nodeoff);

        if (n.lc && order != WalkOrder::Preorder)
            walk(n.lc, order, visitor);
        if (n.rc && order == WalkOrder::Postorder)
            walk(n.rc, order, visitor);

        lca = n.lc ? (lc = get(n.lc), &lc.annotation) : nullptr;
        rca = n.rc ? (rc = get(n.rc), &rc.annotation) : nullptr;
        visitor(n.payload, n.annotation, n.lc, lca, n.rc, rca, nodeoff);

        if (n.lc && order == WalkOrder::Preorder)
            walk(n.lc, order, visitor);
        if (n.rc && order != WalkOrder::Postorder)
            walk(n.rc, order, visitor);
    }

    using SimpleVisitor = std::function<void(const Payload &, off_t)>;

    void visit(off_t nodeoff, SimpleVisitor visitor) const
    {
        if (!nodeoff)
            return;

        const node n = get(nodeoff);
        visit(n.lc, visitor);
        visitor(n.payload, nodeoff);
        visit(n.rc, visitor);
    }
};

#endif // LIBTARMAC_DISKTREE_HH
