#include "includes.h"

/* Given an inode number, return the hash index */
static inline int
lc_inodeHash(ino_t ino) {
    return ino % LC_ICACHE_SIZE;
}

/* Allocate and initialize inode hash table */
void
lc_icache_init(struct fs *fs) {
    struct icache *icache = lc_malloc(fs,
                                      sizeof(struct icache) * LC_ICACHE_SIZE,
                                      LC_MEMTYPE_ICACHE);
    int i;

    for (i = 0; i < LC_ICACHE_SIZE; i++) {
        pthread_mutex_init(&icache[i].ic_lock, NULL);
        icache[i].ic_head = NULL;
    }
    fs->fs_icache = icache;
}

/* Allocate a new inode */
static struct inode *
lc_newInode(struct fs *fs) {
    struct inode *inode;

    inode = lc_malloc(fs, sizeof(struct inode), LC_MEMTYPE_INODE);
    memset(inode, 0, sizeof(struct inode));
    inode->i_block = LC_INVALID_BLOCK;
    inode->i_bmapDirBlock = LC_INVALID_BLOCK;
    inode->i_xattrBlock = LC_INVALID_BLOCK;
    pthread_rwlock_init(&inode->i_rwlock, NULL);

    /* XXX This accounting is not correct after restart */
    __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_inodes, 1);
    __sync_add_and_fetch(&fs->fs_icount, 1);
    return inode;
}

/* Take the lock on inode in the specified mode */
void
lc_inodeLock(struct inode *inode, bool exclusive) {
    if (inode->i_fs->fs_frozen) {
        return;
    }
    if (exclusive) {
        pthread_rwlock_wrlock(&inode->i_rwlock);
    } else {
        pthread_rwlock_rdlock(&inode->i_rwlock);
    }
}

/* Unlock the inode */
void
lc_inodeUnlock(struct inode *inode) {
    if (inode->i_fs->fs_frozen) {
        return;
    }
    pthread_rwlock_unlock(&inode->i_rwlock);
}

/* Add an inode to the hash and file system list */
static void
lc_addInode(struct fs *fs, struct inode *inode) {
    int hash = lc_inodeHash(inode->i_stat.st_ino);

    /* Add the inode to the hash list */
    pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    inode->i_cnext = fs->fs_icache[hash].ic_head;
    fs->fs_icache[hash].ic_head = inode;
    pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
    inode->i_fs = fs;
}

static struct inode *
lc_lookupInodeCache(struct fs *fs, ino_t ino) {
    int hash = lc_inodeHash(ino);
    struct inode *inode;

    if (fs->fs_icache[hash].ic_head == NULL) {
        return NULL;
    }
    /* XXX Locking not needed right now, as inodes are not removed */
    //pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    inode = fs->fs_icache[hash].ic_head;
    while (inode) {
        if (inode->i_stat.st_ino == ino) {
            break;
        }
        inode = inode->i_cnext;
    }
    //pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
    return inode;
}

/* Lookup an inode in the hash list */
static struct inode *
lc_lookupInode(struct fs *fs, ino_t ino) {
    struct gfs *gfs = fs->fs_gfs;

    if (ino == fs->fs_root) {
        return fs->fs_rootInode;
    }
    if (ino == gfs->gfs_snap_root) {
        return gfs->gfs_snap_rootInode;
    }
    return lc_lookupInodeCache(fs, ino);
}

/* Update inode times */
void
lc_updateInodeTimes(struct inode *inode, bool atime, bool mtime, bool ctime) {
    struct timespec tv;

    clock_gettime(CLOCK_REALTIME, &tv);
    if (atime) {
        inode->i_stat.st_atim = tv;
    }
    if (mtime) {
        inode->i_stat.st_mtim = tv;
    }
    if (ctime) {
        inode->i_stat.st_ctim = tv;
    }
}

/* Initialize root inode of a file system */
void
lc_rootInit(struct fs *fs, ino_t root) {
    struct inode *inode = lc_newInode(fs);

    inode->i_stat.st_ino = root;
    inode->i_stat.st_mode = S_IFDIR | 0755;
    inode->i_stat.st_nlink = 2;
    inode->i_stat.st_blksize = LC_BLOCK_SIZE;
    inode->i_parent = root;
    lc_updateInodeTimes(inode, true, true, true);
    inode->i_fs = fs;
    lc_addInode(fs, inode);
    fs->fs_rootInode = inode;
    lc_markInodeDirty(inode, true, true, false, false);
}

/* Set up snapshot root inode */
void
lc_setSnapshotRoot(struct gfs *gfs, ino_t ino) {

    /* Switching layer root is supported just to make tests to run */
    if (gfs->gfs_snap_root) {
        if (gfs->gfs_scount) {
            printf("Warning: Snapshot root changed when snapshots are present\n");
        }
        printf("Switching snapshot root from %ld to %ld\n", gfs->gfs_snap_root, ino);
        gfs->gfs_snap_root = 0;
    }
    gfs->gfs_snap_rootInode = lc_getInode(lc_getGlobalFs(gfs), ino,
                                           NULL, false, false);
    assert(S_ISDIR(gfs->gfs_snap_rootInode->i_stat.st_mode));
    lc_inodeUnlock(gfs->gfs_snap_rootInode);
    gfs->gfs_snap_root = ino;
    printf("snapshot root inode %ld\n", ino);
}

/* Initialize inode table of a file system */
int
lc_readInodes(struct gfs *gfs, struct fs *fs) {
    uint64_t iblock, block = fs->fs_super->sb_inodeBlock;
    struct inode *inode;
    bool flush = false;
    void *ibuf = NULL;
    char *target;
    int i;

    lc_printf("Reading inodes for fs %d %ld\n", fs->fs_gindex, fs->fs_root);
    assert(fs->fs_inodeBlocks == NULL);
    if (block != LC_INVALID_BLOCK) {
        lc_mallocBlockAligned(fs, (void **)&fs->fs_inodeBlocks, false);
        lc_mallocBlockAligned(fs, (void **)&ibuf, false);
    }
    while (block != LC_INVALID_BLOCK) {
        //lc_printf("Reading inode table from block %ld\n", block);
        lc_readBlock(gfs, fs, block, fs->fs_inodeBlocks);
        for (i = 0; i < LC_IBLOCK_MAX; i++) {
            iblock = fs->fs_inodeBlocks->ib_blks[i];
            if (iblock == 0) {
                break;
            }
            if (iblock == LC_INVALID_BLOCK) {

                /* XXX Try to remove these entries */
                continue;
            }
            //lc_printf("Reading inode from block %ld\n", iblock);
            lc_readBlock(gfs, fs, iblock, ibuf);
            inode = ibuf;
            if (inode->i_stat.st_mode == 0) {
                lc_freeLayerMetaBlocks(fs, iblock, 1);
                fs->fs_inodeBlocks->ib_blks[i] = LC_INVALID_BLOCK;
                flush = true;
                continue;
            }
            inode = lc_malloc(fs, sizeof(struct inode), LC_MEMTYPE_INODE);
            __sync_add_and_fetch(&fs->fs_icount, 1);

            /* XXX zero out just necessary fields */
            memset(inode, 0, sizeof(struct inode));
            memcpy(inode, ibuf, sizeof(struct dinode));
            inode->i_block = iblock;
            pthread_rwlock_init(&inode->i_rwlock, NULL);
            lc_addInode(fs, inode);
            if (S_ISREG(inode->i_stat.st_mode)) {
                lc_bmapRead(gfs, fs, inode, ibuf);
            } else if (S_ISDIR(inode->i_stat.st_mode)) {
                lc_dirRead(gfs, fs, inode, ibuf);
            } else if (S_ISLNK(inode->i_stat.st_mode)) {
                inode->i_target = lc_malloc(fs, inode->i_stat.st_size + 1,
                                            LC_MEMTYPE_SYMLINK);
                target = ibuf;
                target += sizeof(struct dinode);
                memcpy(inode->i_target, target, inode->i_stat.st_size);
                inode->i_target[inode->i_stat.st_size] = 0;
            }
            lc_xattrRead(gfs, fs, inode, ibuf);
            if (inode->i_stat.st_ino == fs->fs_root) {
                assert(S_ISDIR(inode->i_stat.st_mode));
                fs->fs_rootInode = inode;
            }
        }
        if (flush) {
            lc_writeBlock(gfs, fs, fs->fs_inodeBlocks, block);
            flush = false;
        }
        block = fs->fs_inodeBlocks->ib_next;
    }
    assert(fs->fs_rootInode != NULL);
    if (fs->fs_inodeBlocks) {
        lc_free(fs, fs->fs_inodeBlocks, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
        fs->fs_inodeBlocks = NULL;
        lc_free(fs, ibuf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    }
    return 0;
}

/* Free an inode and associated resources */
static void
lc_freeInode(struct inode *inode) {
    struct fs *fs = inode->i_fs;

    if (S_ISREG(inode->i_stat.st_mode)) {
        lc_truncPages(inode, 0, false);
    } else if (S_ISDIR(inode->i_stat.st_mode)) {
        lc_dirFree(inode);
    } else if (S_ISLNK(inode->i_stat.st_mode)) {
        if (!inode->i_shared) {
            lc_free(fs, inode->i_target, inode->i_stat.st_size + 1,
                    LC_MEMTYPE_SYMLINK);
        }
        inode->i_target = NULL;
    }
    assert(inode->i_page == NULL);
    assert(inode->i_bmap == NULL);
    assert(inode->i_bcount == 0);
    assert(inode->i_pcount == 0);
    assert(inode->i_dpcount == 0);
    lc_xattrFree(inode);
    pthread_rwlock_destroy(&inode->i_rwlock);
    lc_blockFreeExtents(fs, inode->i_bmapDirExtents, false, false, true);
    lc_blockFreeExtents(fs, inode->i_xattrExtents, false, false, true);
    lc_free(fs, inode, sizeof(struct inode), LC_MEMTYPE_INODE);
}

/* Invalidate dirty inode pages */
void
lc_invalidateInodePages(struct gfs *gfs, struct fs *fs) {
    struct page *page;

    if (fs->fs_inodePagesCount) {
        page = fs->fs_inodePages;
        fs->fs_inodePages = NULL;
        fs->fs_inodePagesCount = 0;
        lc_releasePages(gfs, fs, page);
    }
}

/* Flush dirty inodes */
static void
lc_flushInodePages(struct gfs *gfs, struct fs *fs) {
    //lc_printf("lc_flushInodePages: flushing %ld inodes\n", fs->fs_inodePagesCount);
    lc_flushPageCluster(gfs, fs, fs->fs_inodePages, fs->fs_inodePagesCount);
    fs->fs_inodePages = NULL;
    fs->fs_inodePagesCount = 0;
}

/* Flush a dirty inode to disk */
int
lc_flushInode(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    struct page *page = NULL;
    bool written = false;

    assert(inode->i_fs == fs);
    if (inode->i_xattrdirty) {
        lc_xattrFlush(gfs, fs, inode);
    }

    if (inode->i_bmapdirty) {
        lc_bmapFlush(gfs, fs, inode);
    }

    if (inode->i_dirdirty) {
        lc_dirFlush(gfs, fs, inode);
    }

    /* Write out a dirty inode */
    if (inode->i_dirty) {
        if (inode->i_removed) {
            assert(inode->i_extentLength == 0);

            /* Free metadata blocks allocated to the inode */
            lc_blockFreeExtents(fs, inode->i_bmapDirExtents,
                                true, false, true);
            inode->i_bmapDirExtents = NULL;
            inode->i_bmapDirBlock = LC_INVALID_BLOCK;
            lc_blockFreeExtents(fs, inode->i_xattrExtents,
                                true, false, true);
            inode->i_xattrBlock = LC_INVALID_BLOCK;
            inode->i_xattrExtents = NULL;
        }

        /* An removed inode with a disk copy, needs to be written out so that
         * it would be considered removed when the layer is remounted.
         */
        if (!inode->i_removed || (inode->i_block != LC_INVALID_BLOCK)) {
            if (inode->i_block == LC_INVALID_BLOCK) {
                if ((fs->fs_inodeBlocks == NULL) ||
                    (fs->fs_inodeIndex >= LC_IBLOCK_MAX)) {
                    lc_newInodeBlock(gfs, fs);
                }
                if (fs->fs_blockInodesCount == 0) {
                    fs->fs_blockInodesCount = LC_INODE_CLUSTER_SIZE;
                    fs->fs_blockInodes = lc_blockAllocExact(fs,
                                                       fs->fs_blockInodesCount,
                                                       true, true);
                }
                inode->i_block = fs->fs_blockInodes++;
                fs->fs_blockInodesCount--;
                fs->fs_inodeBlocks->ib_blks[fs->fs_inodeIndex++] = inode->i_block;
            }
            written = true;

            //lc_printf("Writing inode %ld to block %ld\n", inode->i_stat.st_ino, inode->i_block);
            page = lc_getPageNewData(fs, inode->i_block);
            memcpy(page->p_data, &inode->i_dinode, sizeof(struct dinode));
            if (inode->i_removed) {
                ((struct inode *)page->p_data)->i_stat.st_mode = 0;
            }
            if (S_ISLNK(inode->i_stat.st_mode)) {
                memcpy(&page->p_data[sizeof(struct dinode)], inode->i_target,
                       inode->i_stat.st_size);
            }
            page->p_dvalid = 1;
            if (fs->fs_inodePages &&
                (page->p_block != (fs->fs_inodePages->p_block + 1))) {
                lc_flushInodePages(gfs, fs);
            }
            page->p_dnext = fs->fs_inodePages;
            fs->fs_inodePages = page;
            fs->fs_inodePagesCount++;
            if (fs->fs_inodePagesCount >= LC_CLUSTER_SIZE) {
                lc_flushInodePages(gfs, fs);
            }
        }
        inode->i_dirty = false;
    }
    return written ? 1 : 0;
}

/* Sync all dirty inodes */
void
lc_syncInodes(struct gfs *gfs, struct fs *fs) {
    struct inode *inode;
    uint64_t count = 0;
    int i;

    lc_printf("Syncing inodes for fs %d %ld\n", fs->fs_gindex, fs->fs_root);
    for (i = 0; i < LC_ICACHE_SIZE; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode && !fs->fs_removed) {
            if (lc_inodeDirty(inode)) {
                count += lc_flushInode(gfs, fs, inode);
            }
            inode = inode->i_cnext;
        }
    }
    if (fs->fs_inodePagesCount && !fs->fs_removed) {
        lc_flushInodePages(gfs, fs);
    }
    if (!fs->fs_removed) {
        lc_flushInodeBlocks(gfs, fs);
    }
    if (count) {
        __sync_add_and_fetch(&fs->fs_iwrite, count);
    }
}

/* Destroy inodes belong to a file system */
void
lc_destroyInodes(struct fs *fs, bool remove) {
    uint64_t icount = 0, rcount = 0;
    struct inode *inode;
    int i;

    /* Take the inode off the hash list */
    for (i = 0; i < LC_ICACHE_SIZE; i++) {
        /* XXX Lock is not needed as the file system is locked for exclusive
         * access
         * */
        //pthread_mutex_lock(&fs->fs_icache[i].ic_lock);
        while ((inode = fs->fs_icache[i].ic_head)) {
            fs->fs_icache[i].ic_head = inode->i_cnext;
            if (!inode->i_removed) {
                rcount++;
            }
            lc_freeInode(inode);
            icount++;
        }
        assert(fs->fs_icache[i].ic_head == NULL);
        //pthread_mutex_unlock(&fs->fs_icache[i].ic_lock);
        pthread_mutex_destroy(&fs->fs_icache[i].ic_lock);
    }

    /* XXX reuse this cache for another file system */
    lc_free(fs, fs->fs_icache, sizeof(struct icache) * LC_ICACHE_SIZE,
            LC_MEMTYPE_ICACHE);
    if (remove && icount) {
        __sync_sub_and_fetch(&fs->fs_gfs->gfs_super->sb_inodes, rcount);
    }
    if (icount) {
        __sync_sub_and_fetch(&fs->fs_icount, icount);
    }
}

/* Clone an inode from a parent layer */
struct inode *
lc_cloneInode(struct fs *fs, struct inode *parent, ino_t ino) {
    struct inode *inode;

    inode = lc_newInode(fs);
    memcpy(&inode->i_stat, &parent->i_stat, sizeof(struct stat));

    if (S_ISREG(inode->i_stat.st_mode)) {
        assert(parent->i_page == NULL);
        assert(parent->i_dpcount == 0);

        /* Share pages initially */
        if (parent->i_stat.st_blocks) {
            if (parent->i_extentLength) {
                inode->i_extentBlock = parent->i_extentBlock;
                inode->i_extentLength = parent->i_extentLength;
            } else {
                inode->i_bmap = parent->i_bmap;
                inode->i_bcount = parent->i_bcount;
                inode->i_bmapdirty = true;
                inode->i_shared = true;
            }
        } else {
            inode->i_private = true;
        }
    } else if (S_ISDIR(inode->i_stat.st_mode)) {
        if (parent->i_dirent) {
            inode->i_dirent = parent->i_dirent;
            inode->i_shared = true;
            inode->i_dirdirty = true;
        }
    } else if (S_ISLNK(inode->i_stat.st_mode)) {
        inode->i_target = parent->i_target;
        inode->i_shared = true;
    }
    inode->i_parent = (parent->i_parent == parent->i_fs->fs_root) ?
                      fs->fs_root : parent->i_parent;
    lc_xattrCopy(inode, parent);
    lc_addInode(fs, inode);
    inode->i_dirty = true;
    __sync_add_and_fetch(&fs->fs_gfs->gfs_clones, 1);
    return inode;
}

/* Lookup the requested inode in the chain */
static struct inode *
lc_getInodeParent(struct fs *fs, ino_t inum, bool copy) {
    struct inode *inode, *parent;
    struct fs *pfs;

    /* XXX Reduce the time this lock is held */
    pthread_mutex_lock(fs->fs_ilock);
    inode = lc_lookupInodeCache(fs, inum);
    if (inode == NULL) {
        pfs = fs->fs_parent;
        while (pfs) {
            parent = lc_lookupInodeCache(pfs, inum);
            if (parent != NULL) {

                /* Do not clone if the inode is removed in a parent layer */
                if (!parent->i_removed) {

                    /* Clone the inode only when modified */
                    if (copy) {
                        assert(fs->fs_snap == NULL);
                        inode = lc_cloneInode(fs, parent, inum);
                    } else {
                        /* XXX Remember this for future lookup */
                        inode = parent;
                    }
                }
                break;
            }
            pfs = pfs->fs_parent;
        }
    }
    pthread_mutex_unlock(fs->fs_ilock);
    return inode;
}

/* Get an inode locked in the requested mode */
struct inode *
lc_getInode(struct fs *fs, ino_t ino, struct inode *handle,
             bool copy, bool exclusive) {
    ino_t inum = lc_getInodeHandle(ino);
    struct inode *inode;

    assert(!fs->fs_removed);

    /* Check if the file handle points to the inode */
    if (handle) {
        inode = handle;
        if (!copy || (inode->i_fs == fs)) {
            assert(inode->i_stat.st_ino == inum);
            lc_inodeLock(inode, exclusive);
            return inode;
        }
    }

    /* Check if the file system has the inode or not */
    inode = lc_lookupInode(fs, inum);
    if (inode) {
        lc_inodeLock(inode, exclusive);
        return inode;
    }

    /* Lookup inode in the parent chain */
    if (fs->fs_parent) {
        inode = lc_getInodeParent(fs, inum, copy);
    }

    /* Now lock the inode */
    if (inode) {
        lc_inodeLock(inode, exclusive);
    } else {
        lc_printf("Inode is NULL, fs gindex %d root %ld ino %ld\n",
                   fs->fs_gindex, fs->fs_root, ino);
    }
    return inode;
}

/* Allocate a new inode */
ino_t
lc_inodeAlloc(struct fs *fs) {
    return __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_ninode, 1);
}

/* Initialize a newly allocated inode */
struct inode *
lc_inodeInit(struct fs *fs, mode_t mode, uid_t uid, gid_t gid,
              dev_t rdev, ino_t parent, const char *target) {
    struct inode *inode;
    ino_t ino;
    int len;

    ino = lc_inodeAlloc(fs);
    inode = lc_newInode(fs);
    inode->i_stat.st_ino = ino;
    inode->i_stat.st_mode = mode;
    inode->i_stat.st_nlink = S_ISDIR(mode) ? 2 : 1;
    inode->i_stat.st_uid = uid;
    inode->i_stat.st_gid = gid;
    inode->i_stat.st_rdev = rdev;
    inode->i_stat.st_blksize = LC_BLOCK_SIZE;
    inode->i_parent = lc_getInodeHandle(parent);
    inode->i_private = S_ISREG(mode);
    lc_updateInodeTimes(inode, true, true, true);
    if (target != NULL) {
        len = strlen(target);
        inode->i_target = lc_malloc(fs, len + 1, LC_MEMTYPE_SYMLINK);
        memcpy(inode->i_target, target, len);
        inode->i_target[len] = 0;
        inode->i_stat.st_size = len;
    }
    lc_addInode(fs, inode);
    lc_inodeLock(inode, true);
    return inode;
}