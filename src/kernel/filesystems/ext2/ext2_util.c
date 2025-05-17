#include <ext2.h>
#include <malloc.h>
#include <system.h>
#include <util.h>

void ext2BlockFetchInit(Ext2 *ext2, Ext2LookupControl *control) {
  control->tmp1 = (uint32_t *)malloc(ext2->blockSize);
  control->tmp2 = (uint32_t *)malloc(ext2->blockSize);
}

void ext2BlockFetchCleanup(Ext2LookupControl *control) {
  if (control->tmp1)
    free(control->tmp1);
  if (control->tmp2)
    free(control->tmp2);
  control->tmp1Block = 0;
  control->tmp2Block = 0;
}

// this weird-looking function adds whatever overhead is expected for that raw
// size and returns the full thing
size_t ext2BlockSizeCalculate(Ext2 *ext2, size_t raw) {
  uint32_t blocks = DivRoundUp(raw, ext2->blockSize);
  size_t   retblocks = blocks;

  size_t perSpecial = ext2->blockSize / 4;

  size_t base = 12;
  size_t singlyBase = base + perSpecial;
  size_t doublyBase = singlyBase + perSpecial * perSpecial;

  if (blocks < base) {
    // no-op. doesn't need any extra space
  } else if (blocks < singlyBase) {
    retblocks += 1; // singly indirect needs 1 block
  } else if (blocks < doublyBase) {
    // singly + doubly + some singly blocks it points to
    size_t remaining = blocks - singlyBase;

    // one for the (old) singly indirect pointer
    retblocks += 1;

    // one for the doubly indirect pointer
    retblocks += 1;

    // singly indirect blocks needed under the doubly indirect block
    size_t singly_needed = DivRoundUp(remaining, perSpecial);
    retblocks += singly_needed;
  } else
    assert(false);

  return retblocks * ext2->blockSize;
}

uint32_t ext2BlockFetch(Ext2 *ext2, Ext2Inode *ino, uint32_t inodeNum,
                        Ext2LookupControl *control, size_t curr) {
  int result = 0;

  uint32_t group = INODE_TO_BLOCK_GROUP(ext2, inodeNum);
  spinlockCntReadAcquire(&ext2->WLOCKS_BLOCK_BITMAP[group]);

  uint32_t itemsPerBlock = ext2->blockSize / sizeof(uint32_t);
  size_t   baseSingly = 12 + itemsPerBlock;
  size_t   baseDoubly = baseSingly + ext2->blockSize * itemsPerBlock;
  if (curr < 12) {
    result = ino->blocks[curr];
    goto cleanup;
  } else if (curr < baseSingly) {
    if (!ino->blocks[12]) {
      result = 0;
      goto cleanup;
    }
    size_t tmp1block = BLOCK_TO_LBA(ext2, 0, ino->blocks[12]);
    if (control->tmp1Block != tmp1block) {
      control->tmp1Block = tmp1block;
      // control->tmp1 = (uint32_t *)malloc(ext2->blockSize);
      getDiskBytes((void *)control->tmp1, tmp1block,
                   ext2->blockSize / SECTOR_SIZE);
    }
    result = control->tmp1[curr - 12];
    goto cleanup;
  } else if (curr < baseDoubly) {
    if (!ino->blocks[13]) {
      result = 0;
      goto cleanup;
    }
    size_t tmp1block = BLOCK_TO_LBA(ext2, 0, ino->blocks[13]);
    if (control->tmp1Block != tmp1block) {
      control->tmp1Block = tmp1block;
      // control->tmp2 = (uint32_t *)malloc(ext2->blockSize);
      getDiskBytes((void *)control->tmp1, tmp1block,
                   ext2->blockSize / SECTOR_SIZE);
    }

    size_t   at = curr - baseSingly;
    uint32_t index = at / itemsPerBlock;
    uint32_t rem = at % itemsPerBlock;
    size_t   tmp2block = BLOCK_TO_LBA(ext2, 0, control->tmp1[index]);
    if (!control->tmp1[index]) {
      result = 0;
      goto cleanup;
    }
    if (control->tmp2Block != tmp2block) {
      control->tmp2Block = tmp2block;
      getDiskBytes((void *)control->tmp2, tmp2block,
                   ext2->blockSize / SECTOR_SIZE);
    }
    result = control->tmp2[rem];
    goto cleanup;
  }

  debugf("[ext2] TODO! Triply Indirect Block Pointer!\n");
  panic();
  return 0;

cleanup:
  spinlockCntReadRelease(&ext2->WLOCKS_BLOCK_BITMAP[group]);
  return result;
}

void ext2BlockAssign(Ext2 *ext2, Ext2Inode *ino, uint32_t inodeNum,
                     Ext2LookupControl *control, size_t curr, uint32_t val) {
  uint32_t group = INODE_TO_BLOCK_GROUP(ext2, inodeNum);
  spinlockCntWriteAcquire(&ext2->WLOCKS_BLOCK_BITMAP[group]);
  uint32_t itemsPerBlock = ext2->blockSize / sizeof(uint32_t);
  size_t   baseSingly = 12 + itemsPerBlock;
  // size_t   baseDoubly = baseSingly + ext2->blockSize * itemsPerBlock;
  if (curr < 12) {
    ino->blocks[curr] = val;
    ext2InodeModifyM(ext2, inodeNum, ino);
    goto cleanup;
  } else if (curr < baseSingly) {
    bool noninit = false;
    if (!ino->blocks[12]) {
      spinlockCntWriteRelease(&ext2->WLOCKS_BLOCK_BITMAP[group]);
      uint32_t block = ext2BlockFind(ext2, group, 1); // race?
      spinlockCntWriteAcquire(&ext2->WLOCKS_BLOCK_BITMAP[group]);
      ino->blocks[12] = block;
      ext2InodeModifyM(ext2, inodeNum, ino);
      noninit = true;
    }
    size_t tmp1block = BLOCK_TO_LBA(ext2, 0, ino->blocks[12]);
    if (control->tmp1Block != tmp1block) {
      control->tmp1Block = tmp1block;
      // control->tmp1 = (uint32_t *)malloc(ext2->blockSize);
      getDiskBytes((void *)control->tmp1, tmp1block,
                   ext2->blockSize / SECTOR_SIZE);
    }
    if (noninit) // todo: also depend on inode->blockSize globally instead of 0s
      memset(control->tmp1, 0, ext2->blockSize);
    control->tmp1[curr - 12] = val;
    setDiskBytes((void *)control->tmp1, tmp1block,
                 ext2->blockSize / SECTOR_SIZE);
    goto cleanup;
  } /*else if (curr < baseDoubly) {
    if (!ino->blocks[13])
      return 0;
    size_t tmp1block = BLOCK_TO_LBA(ext2, 0, ino->blocks[13]);
    if (control->tmp1Block != tmp1block) {
      control->tmp1Block = tmp1block;
      // control->tmp2 = (uint32_t *)malloc(ext2->blockSize);
      getDiskBytes((void *)control->tmp1, tmp1block,
                   ext2->blockSize / SECTOR_SIZE);
    }

    size_t   at = curr - baseSingly;
    uint32_t index = at / itemsPerBlock;
    uint32_t rem = at % itemsPerBlock;
    size_t   tmp2block = BLOCK_TO_LBA(ext2, 0, control->tmp1[index]);
    if (!control->tmp1[index])
      return 0;
    if (control->tmp2Block != tmp2block) {
      control->tmp2Block = tmp2block;
      getDiskBytes((void *)control->tmp2, tmp2block,
                   ext2->blockSize / SECTOR_SIZE);
    }
    return control->tmp2[rem];
  }*/

  debugf("[ext2::write] TODO! Singly/Doubly/Triply Indirect Block Pointer!\n");
  panic();

cleanup:
  spinlockCntWriteRelease(&ext2->WLOCKS_BLOCK_BITMAP[group]);
}

uint32_t ext2BlockFind(Ext2 *ext2, int groupSuggestion, uint32_t amnt) {
  if (ext2->superblock.free_blocks < amnt)
    goto burn;

  uint32_t suggested = ext2BlockFindL(ext2, groupSuggestion, amnt);
  if (suggested)
    return suggested;

  for (int i = 0; i < ext2->blockGroups; i++) {
    if (i == groupSuggestion)
      continue;

    uint32_t ret = ext2BlockFindL(ext2, i, amnt);
    if (ret)
      return ret;
  }

burn:
  debugf("[ext2] FATAL! Couldn't find blocks! Drive is full! amnt{%d}\n", amnt);
  panic();
  return 0;
}

uint32_t ext2BlockFindL(Ext2 *ext2, int group, uint32_t amnt) {
  if (ext2->bgdts[group].free_blocks < amnt)
    return 0;

  spinlockCntWriteAcquire(&ext2->WLOCKS_BLOCK_BITMAP[group]);

  uint32_t ret = 0;
  uint8_t *buff = malloc(ext2->blockSize);

  getDiskBytes(buff, BLOCK_TO_LBA(ext2, 0, ext2->bgdts[group].block_bitmap),
               ext2->blockSize / SECTOR_SIZE);

  uint32_t foundBlk = 0;
  uint32_t foundAmnt = 0;
  for (int i = 0; i < ext2->blockSize; i++) {
    for (int j = 0; j < 8; j++) {
      if (buff[i] & (1 << j)) {
        foundBlk = i * 8 + j + 1; // next one
        foundAmnt = 0;
      } else {
        foundAmnt++;
        if (foundAmnt >= amnt) {
          ret = foundBlk;
          goto cleanup;
        }
      }
    }
  }

cleanup:
  if (ret) {
    // we found blocks successfully, mark them as allocated
    for (int i = 0; i < amnt; i++) {
      uint32_t where = (foundBlk + i) / 8;
      uint32_t remainder = (foundBlk + i) % 8;
      buff[where] |= (1 << remainder);
    }
    setDiskBytes(buff, BLOCK_TO_LBA(ext2, 0, ext2->bgdts[group].block_bitmap),
                 ext2->blockSize / SECTOR_SIZE);

    // set the bgdt accordingly
    spinlockAcquire(&ext2->LOCK_BGDT_WRITE);
    ext2->bgdts[group].free_blocks -= amnt;
    ext2BgdtPushM(ext2);
    spinlockRelease(&ext2->LOCK_BGDT_WRITE);

    // and the superblock
    spinlockAcquire(&ext2->LOCK_SUPERBLOCK_WRITE);
    ext2->superblock.free_blocks -= amnt;
    ext2SuperblockPushM(ext2);
    spinlockRelease(&ext2->LOCK_SUPERBLOCK_WRITE);
  }

  spinlockCntWriteRelease(&ext2->WLOCKS_BLOCK_BITMAP[group]);
  return ret ? (group * ext2->superblock.blocks_per_group + ret) : 0;
}

void ext2BlockDelete(Ext2 *ext2, uint32_t group, uint32_t index) {
  spinlockCntWriteAcquire(&ext2->WLOCKS_BLOCK_BITMAP[group]);

  uint8_t *buff = malloc(ext2->blockSize);

  getDiskBytes(buff, BLOCK_TO_LBA(ext2, 0, ext2->bgdts[group].block_bitmap),
               ext2->blockSize / SECTOR_SIZE);

  uint32_t where = index / 8;
  uint32_t remainder = index % 8;
  buff[where] &= ~(1 << remainder);
  setDiskBytes(buff, BLOCK_TO_LBA(ext2, 0, ext2->bgdts[group].block_bitmap),
               ext2->blockSize / SECTOR_SIZE);

  // set the bgdt accordingly
  spinlockAcquire(&ext2->LOCK_BGDT_WRITE);
  ext2->bgdts[group].free_blocks++;
  ext2BgdtPushM(ext2);
  spinlockRelease(&ext2->LOCK_BGDT_WRITE);

  // and the superblock
  spinlockAcquire(&ext2->LOCK_SUPERBLOCK_WRITE);
  ext2->superblock.free_blocks++;
  ext2SuperblockPushM(ext2);
  spinlockRelease(&ext2->LOCK_SUPERBLOCK_WRITE);

  spinlockCntWriteRelease(&ext2->WLOCKS_BLOCK_BITMAP[group]);
}

uint32_t *ext2BlockChain(Ext2 *ext2, Ext2OpenFd *fd, size_t curr,
                         size_t blocks) {
  uint32_t *ret = (uint32_t *)malloc((1 + blocks) * sizeof(uint32_t));
  for (int i = 0; i < (1 + blocks); i++) // will take care of curr too
  {
    ret[i] = ext2BlockFetch(ext2, &fd->inode, fd->inodeNum, &fd->lookup, curr);
    curr++;
  }
  return ret;
}

force_inline bool isPowerOf(int n, int power) {
  if (n < 1)
    return false;

  while (n % power == 0)
    n /= power;

  return n == 1;
}

// IMPORTANT! Remember to manually set the spinlock **before** calling
void ext2BgdtPushM(Ext2 *ext2) {
  // the one directly below the superblock
  setDiskBytes((void *)ext2->bgdts, ext2->offsetBGDT,
               DivRoundUp(ext2->blockSize, SECTOR_SIZE));

  for (int i = 1; i < ext2->blockGroups; i++) {
    if (!(i == 0 || i == 1 || isPowerOf(i, 3) || isPowerOf(i, 5) ||
          isPowerOf(i, 7)))
      continue;

    // has a backup/copy...
    setDiskBytes(
        (void *)ext2->bgdts,
        BLOCK_TO_LBA(ext2, 0, i * ext2->superblock.blocks_per_group + 1),
        DivRoundUp(ext2->blockSize, SECTOR_SIZE));
  }
}

// IMPORTANT! Remember to manually set the spinlock **before** calling
void ext2SuperblockPushM(Ext2 *ext2) {
  setDiskBytes((void *)(&ext2->superblock), ext2->offsetSuperblock, 2);

  for (int i = 1; i < ext2->blockGroups; i++) {
    if (!(i == 0 || i == 1 || isPowerOf(i, 3) || isPowerOf(i, 5) ||
          isPowerOf(i, 7)))
      continue;

    setDiskBytes((void *)(&ext2->superblock),
                 BLOCK_TO_LBA(ext2, 0, i * ext2->superblock.blocks_per_group),
                 2);
  }
}
