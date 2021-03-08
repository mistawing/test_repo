// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xk/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>

#include <buf.h>

struct {
  struct buf bufs[40];
  uint size;
  struct sleeplock lock;
  //struct spinlock lock;
} log_cache;

// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Read the super block.
void readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

uint getstartblkno(int dev) {
  struct buf *bp;

  // Go through bitmap starting from last
  for (uint i = sb.inodestart - 1; i >= sb.bmapstart; i--) {
    bp = bread(dev, i);

    // Go through each byte which represents one page
    for (uint j = 0; j < BSIZE; j++) {
      
      // if the page is available, set to unavailable and return
      if (bp->data[j] == 0x00) {
        bp->data[j] = 0xFF;
        bwrite(bp);
        brelse(bp);
        return (sb.nblocks + sb.inodestart) 
               - ((sb.inodestart - 1 - i) * BSIZE)
               - (j + 1) * 8;
      }

    }

    brelse(bp);
  }

  return 0;
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// range of blocks holding the file's content.
//
// The inodes themselves are contained in a file known as the
// inodefile. This allows the number of inodes to grow dynamically
// appending to the end of the inode file. The inodefile has an
// inum of 1 and starts at sb.startinode.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// Since there is no writing to the file system there is no need
// for the callers to worry about coherence between the disk
// and the in memory copy, although that will become important
// if writing to the disk is introduced.
//
// Clients use iload() to populate an inode with valid information
// from the disk. idup() can be used to add an in memory reference
// to and inode. iput() will decrement the in memory reference count
// and will free the inode if there are no more references to it,
// freeing up space in the cache for the inode to be used again.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
  struct inode inodefile;
} icache;

// Find the inode file on the disk and load it into memory
// should only be called once, but is idempotent.
static void init_inodefile(int dev) {
  struct buf *b;
  struct dinode di;

  b = bread(dev, sb.inodestart);
  memmove(&di, b->data, sizeof(struct dinode));

  icache.inodefile.inum = INODEFILEINO;
  icache.inodefile.dev = dev;
  icache.inodefile.type = di.type;
  icache.inodefile.devid = di.devid;
  icache.inodefile.size = di.size;
  icache.inodefile.data[0] = di.data[0];
  for (int i = 1; i < EXTENT_N; i++) {
    di.data[i].startblkno = 0;
    di.data[i].nblocks = 0;
    icache.inodefile.data[i] = di.data[i];
  }

  brelse(b);
}

void iinit(int dev) {
  int i;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
  initsleeplock(&icache.inodefile.lock, "inodefile");
  initsleeplock(&log_cache.lock, "log cache");
  //initlock(&log_cache.lock, "log cache");

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d bmap start %d inodestart %d\n", sb.size,
          sb.nblocks, sb.bmapstart, sb.inodestart);
  log_recover();
  init_inodefile(dev);
}

static void read_dinode(uint inum, struct dinode *dip) {
  readi(&icache.inodefile, (char *)dip, INODEOFF(inum), sizeof(*dip));
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not read
// the inode from from disk.
static struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *empty;
  struct dinode dip;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->ref = 1;
  ip->dev = dev;
  ip->inum = inum;

  release(&icache.lock);

  read_dinode(ip->inum, &dip);
  ip->type = dip.type;
  ip->devid = dip.devid;
  ip->size = dip.size;
  for (int i = 0; i < EXTENT_N; i++) {
    ip->data[i] = dip.data[i];
  }

  if (ip->type == 0)
    panic("iget: no type");

  return ip;
}

struct inode *icreate(char *filepath) {
  // Get inodefile
  struct inode *inodefile = iget(ROOTDEV, INODEFILEINO);

  acquiresleep(&(inodefile->lock));

  // Create new dinode to append to inodefile
  struct dinode din;
  din.type = T_FILE;
  din.devid = T_DEV;
  din.size = 0;
  for (int i = 0; i < EXTENT_N; i++) {
    din.data[i].startblkno = 0;
    din.data[i].nblocks = 0;
  }

  // Append dinode
  if (writei(inodefile, (char *) &din, inodefile->size, sizeof(struct dinode)) < 0)
    cprintf("failled to add dinode in sys_open\n");

  // Get rootino
  struct inode *rootino = iget(ROOTDEV, ROOTINO);

  // Create new dirent to append to rootino
  struct dirent *dir;
  dir->inum = inodefile->size / sizeof(struct dinode);
  safestrcpy(dir->name, filepath, DIRSIZ);

  // Append dirent
  if (writei(rootino, (char *) dir, rootino->size, sizeof(struct dirent)) < 0)
    cprintf("failed to add dirent in sys_open\n");

  inodefile->size += sizeof(struct dinode);
  rootino->size += sizeof(struct dirent);

  updatei(inodefile);
  updatei(rootino);
  icache.inodefile = *inodefile; 

  struct inode *ind = iget(ROOTDEV, dir->inum);

  releasesleep(&(inodefile->lock));

  return ind;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
void irelease(struct inode *ip) {
  acquire(&icache.lock);
  // inode has no links and no other references release
  if (ip->ref == 1)
    ip->type = 0;
  ip->ref--;
  release(&icache.lock);
}

// Copy stat information from inode.
void stati(struct inode *ip, struct stat *st) {
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->size = ip->size;
}

uint getCapacity(struct inode *ip) {
  uint capacity = 0;
  for (int i = 0; i < EXTENT_N; i++) {
    capacity += ip->data[i].nblocks * BSIZE;
  }
  return capacity;
}

// Read data from inode.
int readi(struct inode *ip, char *dst, uint off, uint n) {

  uint tot, m;
  struct buf *bp;

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].read)
      return -1;
    return devsw[ip->devid].read(ip, dst, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > ip->size)
    n = ip->size - off;

  struct extent *data = ip->data;
  uint size = 0;
  m = 0;

  // Move to the extent where offset is located.
  while (size + data->nblocks * BSIZE < off) {
    size += data->nblocks * BSIZE;
    data++;
  }

  // Offset for the specific extent.
  size = off - size;

  for (tot = 0; tot < n; tot += m, off += m, dst += m, size += m) {
    // There are still bytes to read in this extent 
    if (size < data->nblocks * BSIZE) {

      bp = bread(ip->dev, data->startblkno + size / BSIZE);
      m = min(n - tot, BSIZE - size % BSIZE);
      memmove(dst, bp->data + size % BSIZE, m);
      brelse(bp);
    } else {
      data++;
      m = 0;
      size = off - size;
    }
  }

  return n;
}

int readp(struct pipe *ip, char *dst, uint off, uint n) {
  int res = 0;

  while (ip->front == ip->tail &&
         ftable.valid_flags[ip->write_fd] != 0) {
    if (myproc()->killed != 0) {
      return -1;
    }

    wakeup(&ip->tail);
    sleep(&ip->front, &ip->lock);
  }

  for(int i = 0; i < n; i++) {
    if (myproc()->killed != 0) {
      return -1;
    }

    if (ip->front == ip->tail) {
      break;
    }

    dst[i] = ip->buf[ip->front % SIZE];
    ip->front++;
    res++;
  }

  wakeup(&ip->tail);
  return res;
}

int appendi(struct inode *ip, char *src, uint append) {
  uint tot, m;
  struct buf *bp;

  struct extent *data = ip->data;
  uint size = 0;

  while(data->nblocks != 0) {
    data++;
  }

  for (tot = 0; tot < append; tot += m, src += m, size += m, data++) {
    m = 0;

    if (data->nblocks == 0) {
      data->startblkno = getstartblkno(ip->dev);
      data->nblocks = 8;
    }

    if (size < data->nblocks * BSIZE) {
      bp = bread(ip->dev, data->startblkno + size / BSIZE);
      m = min(append - tot, BSIZE - size % BSIZE);
      memmove(bp->data + size % BSIZE, src, m);
      log_write(bp);
      brelse(bp);
    } else {
      data++;
      m = 0;
      size = 0;
    }
  }

  log_commit_tx();
  return append;
}

void updatei(struct inode *ip) {
  struct inode *inodefile = iget(ROOTDEV, INODEFILEINO);
  struct dinode curr_dinode;
  read_dinode(ip->inum, &curr_dinode);

  if (curr_dinode.size != ip->size) {

    curr_dinode.size = ip->size;

    for (int i = 0; i < EXTENT_N; i++) {
      curr_dinode.data[i].startblkno = ip->data[i].startblkno;
      curr_dinode.data[i].nblocks = ip->data[i].nblocks;
    }

    writei(inodefile, (char *)&curr_dinode, ip->inum * sizeof(struct dinode),
           sizeof(struct dinode));
  }

}

// Write data to inode.
int writei(struct inode *ip, char *src, uint off, uint n) {

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].write)
      return -1;
    return devsw[ip->devid].write(ip, src, n);
  }

  uint append = 0; 
  uint capacity = getCapacity(ip);
  if (off + n < off) 
    return -1;
  if (off + n > capacity) {
    append = (off + n) - capacity;
    n = capacity - off;
  }

  uint tot, m;
  struct buf *bp;

  struct extent *data = ip->data;
  uint size = 0;

  while (size + data->nblocks * BSIZE < off) {
    size += data->nblocks * BSIZE;
    data++;
  }

  size = off - size;

  for (tot = 0; tot < n; tot += m, off += m, src += m, size += m) {
    m = 0;

    if (size < data->nblocks * BSIZE) {
      bp = bread(ip->dev, data->startblkno + size / BSIZE);
      m = min(n - tot, BSIZE - size % BSIZE);
      memmove(bp->data + size % BSIZE, src, m);
      log_write(bp);
      brelse(bp);
    } else {
      data++;
      m = 0;
      size = off - size;
    }
  }

  if (append > 0) {
    return n + appendi(ip, src, append);
  }

  log_commit_tx();
  return n;
}

int writep(struct pipe *ip, char *src, uint off, uint n) {
  int res = 0;

  for(int i = 0; i < n; i++) {
    while((ip->tail - ip->front) == SIZE) {
      if (ftable.valid_flags[ip->read_fd] == 0) {
        return -1;
      }

      if (myproc()->killed != 0) {
        return -1;
      }

      wakeup(&ip->front);
      sleep(&ip->tail, &ip->lock);
    }

    ip->buf[ip->tail % SIZE] = src[i];
    ip->tail++;
    res++;
  }

  wakeup(&ip->front);
  return res;
}

void log_write(struct buf *b) {
  acquiresleep(&log_cache.lock);

  // Add block to cache
  log_cache.bufs[log_cache.size] = *b;
  log_cache.size++;

  b->flags |= B_DIRTY;

  releasesleep(&log_cache.lock);
}

void log_commit_tx() {
  acquiresleep(&log_cache.lock);
  // Get commit block
  struct buf *commit_buf = bread(ROOTDEV, sb.logstart);
  struct commit_block cb;
  memmove(&cb, commit_buf->data, BSIZE);
  brelse(commit_buf);

  // Write given bufs into log region while updating commit block copy
  for (int i = 0; i < log_cache.size; i++) {
    cb.dst_blocknos[i] = log_cache.bufs[i].blockno;
    cb.size++;
    struct buf *log_buf = bread(ROOTDEV, sb.logstart + i + 1);
    memmove(log_buf->data, log_cache.bufs[i].data, BSIZE);
    bwrite(log_buf);
    brelse(log_buf);
  }

  // Empty log cache
  memset(log_cache.bufs, 0, sizeof(struct buf) * 40);
  log_cache.size = 0;

  // Update commit block to reflect newly added block to log region
  commit_buf = bread(ROOTDEV, sb.logstart);
  cb.commit_flag = 1;
  memmove(commit_buf->data, &cb, BSIZE);
  bwrite(commit_buf);
  brelse(commit_buf);

  releasesleep(&log_cache.lock);

  log_recover();
}

void log_recover() {
  acquiresleep(&log_cache.lock);

  // Get commit block 
  struct buf *commit_buf = bread(ROOTDEV, sb.logstart);
  struct commit_block cb;
  memmove(&cb, commit_buf->data, BSIZE);
  brelse(commit_buf);

  if (cb.commit_flag == 1) {
    // Get blocks from log region and bwrite
    for (int i = 0; i < cb.size; i++) {
      // Get block from log region
      struct buf *src = bread(ROOTDEV, sb.logstart + i + 1);
      brelse(src);
      // Get actual block
      struct buf *dst = bread(ROOTDEV, cb.dst_blocknos[i]);

      memmove(dst->data, src->data, BSIZE);
      bwrite(dst);
      brelse(dst);
    }
  
    // Clear commit block
    commit_buf = bread(ROOTDEV, sb.logstart);
    memset(commit_buf->data, 0, BSIZE);
    bwrite(commit_buf);
    brelse(commit_buf);
  }

  releasesleep(&log_cache.lock);
}

// Directories

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

struct inode *rootlookup(char *name) {
  return dirlookup(namei("/"), name, 0);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0) {
      // entry matches path element
      if (poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;

  if (*path == '/') {
    ip = iget(ROOTDEV, ROOTINO);
  } else {
    ip = idup(namei("/"));
  }

  while ((path = skipelem(path, name)) != 0) {
    if (ip->type != T_DIR)
      goto notfound;

    // Stop one level early.
    if (nameiparent && *path == '\0')
      return ip;

    if ((next = dirlookup(ip, name, 0)) == 0)
      goto notfound;

    irelease(ip);
    ip = next;
  }
  if (nameiparent)
    goto notfound;

  return ip;

notfound:
  irelease(ip);
  return 0;
}

struct inode *namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}
