/*
 * Copyright(C) 2011-2016 Pedro H. Penna <pedrohenriquepenna@gmail.com>
 * 
 * This file is part of Nanvix.
 * 
 * Nanvix is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Nanvix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Nanvix. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 * 
 * @brief Inode module implementation.
 */

#include <nanvix/clock.h>
#include <nanvix/config.h>
#include <nanvix/const.h>
#include <nanvix/dev.h>
#include <nanvix/fs.h>
#include <nanvix/klib.h>
#include <nanvix/mm.h>
#include <errno.h>
#include <limits.h>
#include "fs.h"
#include "inode_minix.h"

/* Number of inodes per block. */
#define INODES_PER_BLOCK (BLOCK_SIZE/sizeof(struct d_inode))

/**
 * @brief In-core inodes table.
 */
PRIVATE struct inode inodes[NR_INODES];

/**
 * @brief Hash table size.
 */
#define HASHTAB_SIZE 227

/* Free inodes. */
PRIVATE struct inode *free_inodes = NULL;

/* Inodes hash table. */
PRIVATE struct inode *hashtab[HASHTAB_SIZE];

/*Table of the files system.	*/
PRIVATE struct file_system_type *fileSystemTable [NR_FILE_SYSTEM] = {
	NULL
};

/*Table of the mount point.	*/
PRIVATE struct mountingPoint mountTable [NR_MOUNTING_POINT];

/**
 * @brief Hash function for the inode cache.
 */
#define HASH(dev, num) \
	(((dev)^(num))%HASHTAB_SIZE)

/**
 * @brief Insert a file system in the fileSystemTable.
 */
PUBLIC int fs_register( int nb , struct file_system_type * fs )
{
	if (nb >= NR_FILE_SYSTEM)
		return (-EINVAL);
	
	/* Fs already registered? */
	if (fileSystemTable[nb] != NULL)
		return (-EBUSY);
	
	/* Register fs */
	 fileSystemTable[nb] = fs;
	
	return (0);
}

/**
 * @brief found a free spot in the mouting table
 * 
 * @returns the indice in mouting table of the free spot
 */
PRIVATE int available_mouting_point (void)
{
	for (int i=0; i<NR_MOUNTING_POINT; i++)
	{
		if (mountTable[i].free)
			return i;
	}
	kprintf ("No more space on mountTable");
	return -1;
}

/**
 * @brief Search if an inode is present in the mouting table as an mouting point
 * 
 * @returns Return a pointeur to the mouting point or NULL
 */
PRIVATE struct mountingPoint * belong_mounting_table(int i_num)
{	
	for (int i=0; i<NR_MOUNTING_POINT; i++)
	{
		if (!mountTable[i].free && mountTable[i].no_inode_mount==i_num)
			return &mountTable[i];
	}
	return NULL;
}

/**
 * @brief Search for the file system of a device in the mounting table.
 * 
 * @returns a pointeur to the file system of the device or null if the device is not in the mouting table.
 */
PRIVATE struct file_system_type * fs_from_device (dev_t dev){
	for (int i=0; i<NR_MOUNTING_POINT; i++)
	{
		if (!mountTable[i].free && mountTable[i].dev==dev)
			return mountTable[i].fs;
	}
	kpanic (" looking for device %i but not founded it \n", dev);
	return NULL;
}

/**
 * @brief Search if an inode is present in the mouting table as an file_system_root, don't look the first entry.
 * 
 * @returns Return a pointeur to the mouting point or NULL
 */
PRIVATE struct mountingPoint * is_root_fs(int i_num)
{	
	for (int i=1; i<NR_MOUNTING_POINT; i++)
	{
		if (!mountTable[i].free && mountTable[i].no_inode_root_fs==i_num)
			return &mountTable[i];
	}
	return NULL;
}

/**
 * @brief mount a device on a directory
 * 
 * @details insert a new mouting point in the mouting table
 * 
 * @returns return 0 in sucess, and 1 if something went wrong
 *
 *@ todo : their is a probleme is the function mount try to acess to the two same inode, 
 *         do we have to check that we don't mount two diffrent device on the same directory
 */
PUBLIC int mount (char* device, char* mountPoint)
{
	struct inode *inode_root_fs=NULL;
	struct inode *inode_mount= NULL;
	int ind_mp;
	struct file_system_type * fs;
	superblock_t sb;

	/*get a free mouting point in the mounting point table*/
	ind_mp=available_mouting_point();

	/* their is no more space available on the mounting point table*/
	if ((ind_mp<0) ||(ind_mp >NR_MOUNTING_POINT)){
		return 1;
	}
	/* get the root inode of the file systeme*/
	inode_root_fs=inode_name(device);

	/*Problem with root inode */
	if (inode_root_fs==NULL)
	{
		kprintf("device inode not found\n");
		return 1;
	}

	/*check if it's a device*/
	if (!S_ISCHR(inode_root_fs->mode) && !S_ISBLK(inode_root_fs->mode)&&inode_root_fs->num!=1)
	{
		kprintf ("what you provide is not a device\n");
		goto error;
	}
	inode_unlock(inode_root_fs);
	
	/*get the inode of the mount point*/
	inode_mount =inode_name(mountPoint);
		
	/*Problem with mount inode */
	if (inode_mount==NULL)
	{
		kprintf("mount inode not found\n");
		goto error;
	}
	/*Check if the mount inode is a directory*/
	if (!S_ISDIR(inode_mount->mode))
	{
		kprintf("mount inode is not a directory\n");
		goto error;
	}

	/*Check if the mount inode is already in the mountable*/
	if (belong_mounting_table(inode_mount->num)!=NULL)
	{
		kprintf("an other device is already mount on this directory\n");
		goto error;
	}

	/*search witch file system is on the device*/
	for(int i=0; i<NR_FILE_SYSTEM; i++){
		if (fileSystemTable[i]!=NULL)
		{
			sb=superblock_read(inode_root_fs->dev);
			if (sb!=NULL)
			{
				fs=fileSystemTable[i];
				goto found;
			}
		}
	}
	kprintf("The file system of the device is not reconized\n");
	goto error;

found:
	mountTable[ind_mp].mountPoint=mountPoint;
	mountTable[ind_mp].dev=inode_root_fs->dev;
	mountTable[ind_mp].fs= fs;
	mountTable[ind_mp].no_inode_root_fs=inode_root_fs->num;
	mountTable[ind_mp].no_inode_mount=inode_mount->num;
	mountTable[ind_mp].free= 0;
	
	inode_unlock(inode_mount);
	return 0;
error:
	if (inode_mount != NULL)
	{
		inode_unlock(inode_mount);
	}
	
	if(inode_root_fs!= NULL ){
		inode_unlock(inode_root_fs);
	}
		
	return 1;
}

/**
 * @brief unmount a device on a directory
 * 
 * @details remove a mouting point of the mouting table
 * 
 * @returns return 0 in sucess, and 1 if something went wrong
 *
 *@ todo : their is a probleme if the function umount try to acess to the two same inode
 */

PUBLIC int unmount (char * device , char * mountPoint){
	struct inode *inode_root_fs;
	struct inode *inode_mount;
	int ind;
		
	/* get the root inode of the file systeme*/
	inode_root_fs=inode_name(device);

	/*Problem with root inode */
	if (inode_root_fs==NULL)
	{
		kprintf("device inode not found\n");
		return 1;
	}

	/*check if it's a device*/
	if (!S_ISCHR(inode_root_fs->mode) && !S_ISBLK(inode_root_fs->mode))
	{
		kprintf ("what you provide is not a device\n");
		goto error;
	}
	
	/*get the inode of the mount point*/
	inode_mount =inode_name(mountPoint);
		
	/*Problem with mount inode */
	if (inode_mount==NULL)
	{
		kprintf("mount inode not found\n");
		goto error;
	}
		
	/*Check if the mount inode is a directory*/
	if (!S_ISDIR(inode_mount->mode))
	{
		kprintf("mount inode is not a directory\n");
		goto error;
	}

	/*look in the mounting table the mounting point to remove.*/

	for (int i=0; i<NR_MOUNTING_POINT; i++)
	{
		if (!mountTable[i].free
			&& mountTable[i].dev==inode_root_fs->dev
			&& mountTable[i].no_inode_mount==inode_mount->num
			&& mountTable[i].no_inode_root_fs==inode_root_fs->num)
		{
			ind=i;
			goto found2;
		}
	}
	kprintf("this mounting point does not exist in the mount table\n");
	goto error;
found2: 
	mountTable[ind].free=1;
	inode_unlock(inode_root_fs);
	inode_unlock(inode_mount);
	return 0;
error:
	if (inode_mount != NULL)
		inode_unlock(inode_mount);
	if(inode_root_fs!= NULL)
		inode_unlock(inode_root_fs);
	return 1;

}

/**
 * @brief Evicts an free inode from the inode cache
 * 
 * @details Searches the inode cache for a free inode and returns it.
 * 
 * @returns If a free inode is found, that inode is locked and then returned.
 *          However, no there is no free inode, a #NULL pointer is returned
 *          instead.
 */
PRIVATE struct inode *inode_cache_evict(void)
{
	struct inode *ip;
	
	/*
	 * No free inodes. 
	 * If this happens too often, it 
	 * may indicate that inode cache
	 * is too small.
	 */
	if (free_inodes == NULL)
	{
		kprintf("fs: inode table overflow");
		return (NULL);
	}
	
	/* Remove inode from free list. */
	ip = free_inodes;
	free_inodes = free_inodes->free_next;
	
	ip->count++;
	inode_lock(ip);
	
	return (ip);
}

/**
 * @brief Inserts an inode in the inode cache.
 * 
 * @details Inserts the inode pointed to by @p ip in the inode cache.
 * 
 * @param ip Inode to be inserted back in the inode cache.
 * 
 * @note The inode must be locked.
 */
PRIVATE void inode_cache_insert(struct inode *ip)
{
	unsigned i;
	
	i = HASH(ip->dev, ip->num);
	
	/* Insert the inode in the hash table. */
	ip->hash_next = hashtab[i];
	ip->hash_prev = NULL;
	hashtab[i] = ip;
	if (ip->hash_next != NULL)
		ip->hash_next->hash_prev = ip;
}

/**
 * @brief Removes an inode from the inode cache.
 * 
 * @details Removes the inode pointed to by @p ip form the inode cache.
 * 
 * @param ip Inode to be removed from the cache.
 * 
 * @note The inode must be locked.
 */
PRIVATE void inode_cache_remove(struct inode *ip)
{
	/* Remove inode from the hash table. */
	if (ip->hash_prev != NULL)
		ip->hash_prev->hash_next = ip->hash_next;
	else
		hashtab[HASH(ip->dev, ip->num)] = ip->hash_next;
	if (ip->hash_next != NULL)
		ip->hash_next->hash_prev = ip->hash_prev;
}

/**
 * @brief Writes an inode to disk.
 * 
 * @details Writes the inode pointed to by @p ip to disk.
 * 
 * @param ip Inode to be written to disk.
 * 
 * @note The inode must be locked.
 */
PRIVATE void inode_write(struct inode *ip, struct file_system_type * fs)
{
	/* Invalid fs. */
	if (fs == NULL)
		kpanic("file system not inisialized");
	
	/* Operation not supported. */
	if (fs->so->inode_write == NULL)
		kpanic("operation not supported by the file system");

	fs->so->inode_write(ip);
}

/**
 * @brief Reads an inode from the disk.
 * 
 * @details Reads the inode with number @p num from the device @p dev.
 * 
 * @param dev Device where the inode is located.
 * @param num Number of the inode that shall be read.
 * 
 * @returns Upon successful completion a pointer to a in-core inode is returned.
 *          In this case, the inode is ensured to be locked. Upon failure, a
 *          #NULL pointer is returned instead.
 * 
 * @note The device number must be valid.
 * @note The inode number must be valid.
 */
PRIVATE struct inode *inode_read(dev_t dev, ino_t num, struct file_system_type * fs)
{
	struct inode *ip;      /* In-core inode. */
	/* Get a free in-core inode. */
	ip = inode_cache_evict();

	if (ip == NULL)
		return (NULL);

	/* Read inode. */

	/* Invalid fs. */
	if (fs == NULL)
		kpanic("file system not initialized");
	
	/* Operation not supported. */
	if (fs->so->inode_read == NULL)
		kpanic("operation not supported by the file system");

	if (fs->so->inode_read(dev,num,ip)){
		return NULL;
	}
	return ip;
}

/**
 * @brief Frees an inode.
 * 
 * @details Frees the inode pointed to by @p ip.
 * 
 * @details The inode must be locked.
 */
PRIVATE void inode_free(struct inode *ip, struct file_system_type * fs)
{
	/* Invalid fs. */
	if (fs== NULL)
		kpanic("file system not initialized");
	
	/* Operation not supported. */
	if (fs->so->inode_free == NULL)
		kpanic("operation not supported by the file system");

	fs->so->inode_free(ip);
}

/**
 * @brief Locks an inode.
 * 
 * @details Locks the inode pointed to by @p ip.
 */
PUBLIC void inode_lock(struct inode *ip)
{
	while (ip->flags & INODE_LOCKED)
		sleep(&ip->chain, PRIO_INODE);
	ip->flags |= INODE_LOCKED;
}

/**
 * @brief Unlocks an inode.
 * 
 * @details Unlocks the inode pointed to by @p ip.
 * 
 * @param ip Inode to be unlocked.
 * 
 * @note The inode must be locked.
 */
PUBLIC void inode_unlock(struct inode *ip)
{
	wakeup(&ip->chain);
	ip->flags &= ~INODE_LOCKED;
}

/**
 * @brief Synchronizes the in-core inode table.
 * 
 * @details Synchronizes the in-core inode table by flushing all valid inodes 
 *          onto underlying devices.
 */
PUBLIC void inode_sync(void)
{
	struct file_system_type * fs;
	/* Write valid inodes to disk. */
	for (struct inode *ip = &inodes[0]; ip < &inodes[NR_INODES]; ip++)
	{
		inode_lock(ip);
		
		/* Write only valid inodes. */
		if (ip->flags & INODE_VALID)
		{
			if (!(ip->flags & INODE_PIPE))
			{
				fs = fs_from_device(ip->dev);
				if (fs== NULL)
					kpanic ("file system not reconized dans inode_sync");
				inode_write(ip,fs);
			}
		}
		
		inode_unlock(ip);
	}
}

/**
 * @brief Truncates an inode.
 * 
 * @details Truncates the inode pointed to by @p ip by freeing all underling 
 *          blocks.
 * 
 * @param ip Inode that shall be truncated.
 * 
 * @note The inode must be locked. 
 */
PUBLIC void inode_truncate(struct inode *ip)
{
	struct file_system_type *fs;
	fs = fs_from_device(ip->dev);
	if (fs== NULL)
		kpanic ("file system not reconized dans inode_truncate");
	/* Invalid fs. */
	if (fs== NULL)
		kpanic("file system not initialized");
	
	/* Operation not supported. */
	if (fs->so->inode_truncate == NULL)
		kpanic("operation not supported by the file system");

	fs->so->inode_truncate(ip);
}

/**
 * @brief Allocates an inode.
 * 
 * @details Allocates an inode in the file system that is associated to the 
 *          superblock pointed to by @p sb.
 * 
 * @param sb Superblock where the inode shall be allocated.
 * 
 * @returns Upon successful completion, a pointed to the inode is returned. In 
 *          this case, the inode is ensured to be locked. Upon failure, a #NULL
 *          pointer is returned instead.
 * 
 * @note The superblock must not be locked.
 * 
 * @todo Use isearch.
 */
PUBLIC struct inode *inode_alloc (struct superblock *sb)
{
	struct inode *ip;

	/* Get a free inode. */
	ip = inode_cache_evict();
		if (ip == NULL)
		return (NULL);

	/* Verifie the superblock */
	if (sb==NULL){
		kpanic("not valid superblock");
	}

	if (sb->so==NULL){
		kpanic("no supper operation in the superblock");
	}

	/* Allocate inode. */
	
	/* Operation not supported. */
	if (sb->so->inode_alloc == NULL)
		kpanic("operation not supported by the file system");

	if (sb->so->inode_alloc(sb,ip)){
		return NULL;
	}
	inode_touch(ip);
	inode_cache_insert(ip);
	return (ip);
}



/**
 * @brief Gets an inode.
 * 
 * @details Gets the inode with number @p num from the device @p dev.
 * 
 * @param dev Device where the inode is located.
 * @param num Number of the inode.
 * 
 * @returns Upon successful completion, a pointer to the inode is returned. In
 *          this case, the inode is ensured to be locked. Upon failure, a #NULL
 *          pointer is returned instead.
 * 
 * @note The device number must be valid.
 * @note The inode number must be valid.
 */
PUBLIC struct inode *inode_get(dev_t dev, ino_t num)
{
	struct inode *ip;
	struct file_system_type * fs;

repeat:

	/* Search in the hash table. */
	for (ip = hashtab[HASH(dev, num)]; ip != NULL; ip = ip->hash_next)
	{
		/* Not found. */
		if (ip->dev != dev || ip->num != num)
			continue;

		/* Inode is locked. */
		if (ip->flags & INODE_LOCKED)
		{
			sleep(&ip->chain, PRIO_INODE);
			goto repeat;
		}
		
		/* Cross mount point. */
		if (ip->flags & INODE_MOUNT)
		{
			 dev = ip->dev;
			 goto repeat;
		}
		
		ip->count++;
		inode_lock(ip);
		
		return (ip);
	}
	
	/* Read inode. */
	fs = fs_from_device(dev);
	if (fs== NULL)
		kpanic ("file system not reconized");
	ip = inode_read(dev, num,fs);
	if (ip == NULL)
		return (NULL);
	
	inode_cache_insert(ip);
	
	return (ip);
}

/*
 * Gets a pipe inode.
 */
PUBLIC struct inode *inode_pipe(void)
{
	void *pipe;          /* Pipe page.  */
	struct inode *inode; /* Pipe inode. */
	
	pipe = getkpg(0);
	
	/* Failed to get pipe page. */
	if (pipe == NULL)
		goto error0;
	
	inode = inode_cache_evict();

	/* No free inode. */
	if (inode == NULL)
		goto error1;

	/* Initialize inode. */
	inode->mode = MAY_READ | MAY_WRITE | S_IFIFO;
	inode->nlinks = 0;
	inode->uid = curr_proc->uid;
	inode->gid = curr_proc->gid;
	inode->size = PAGE_SIZE;
	inode->time = CURRENT_TIME;
	inode->dev = NULL_DEV;
	inode->num = INODE_NULL;
	inode->count = 2;
	inode->flags |= ~(INODE_DIRTY | INODE_MOUNT) & (INODE_VALID | INODE_PIPE);
	inode->pipe = pipe;
	inode->head = 0;
	inode->tail = 0;
	
	return (inode);

error1:
	putkpg(pipe);
error0:
	return (NULL);
}

/**
 * @brief Updates the time stamp of an inode.
 * 
 * @details Updates the time stamp of the inode pointed to by @p ip to current
 *          time.
 * 
 * @param ip Inode to be touched.
 * 
 * @note The inode must be locked.
 */
PUBLIC inline void inode_touch(struct inode *ip)
{
	ip->time = CURRENT_TIME;
	ip->flags |= INODE_DIRTY;
}

/**
 * @brief Releases a in-core inode.
 * 
 * @details Releases the inc-core inode pointed to by @p ip. If its reference 
 *          count drops to zero, all underlying resources are freed and then the
 *          inode is marked as invalid.
 * 
 * @param ip Inode that shall be released.
 * 
 * @note The inode must be valid
 * @note The inode must be locked.
 */
PUBLIC void inode_put(struct inode *ip)
{
	struct file_system_type *fs;
	/* Double free. */
	if (ip->count == 0)
		kpanic("freeing inode twice");
	
	/* Release underlying resources. */
	if (--ip->count == 0)
	{
		/* Pipe inode. */
		if (ip->flags & INODE_PIPE)
			putkpg(ip->pipe);
			
		/* File inode. */
		else
		{		
			/* Free underlying disk blocks. */
			fs = fs_from_device(ip->dev);
			if (fs== NULL)
				kpanic ("file system not reconized");

			if (ip->nlinks == 0)
			{
				inode_free(ip,fs);
				inode_truncate(ip);
			}
			inode_write(ip,fs);
			inode_cache_remove(ip);
		}
		
		/* Insert inode in the free list. */
		ip->free_next = free_inodes;
		free_inodes = ip;
		ip->flags &= ~INODE_VALID;
	}
	inode_unlock(ip);
}

/**
 * @brief Breaks a path
 * 
 * @details Parses the path pointed to by @p pathname extracting the first
 *          path-part from it. The path-part is stored in the array pointed to
 *          by @p filename.
 * 
 * @param pathname Path that shall be broken.
 * @param filename Array where the first path-part should be save.
 * 
 * @returns Upon successful completion, a pointer to the second path-part is 
 *          returned, so a new call to this function can be made to parse the
 *          remainder of the path. Upon failure, a #NULL pointer is returned 
 *          instead.
 */
PRIVATE const char *break_path(const char *pathname, char *filename)
{
	char *p2;       /* Write pointer. */
	const char *p1; /* Read pointer.  */
	
	p1 = pathname;
	p2 = filename;
	
	/* Skip those. */
	while (*p1 == '/')
		p1++;
	
	/* Get file name. */
	while ((*p1 != '\0') && (*p1 != '/'))
	{
		/* File name too long. */
		if ((p2 - filename) > NAME_MAX)
			return (NULL);
		
		*p2++ = *p1++;
	}
	
	*p2 = '\0';
	
	return (p1);
}

/*
 * Gets inode of the topmost directory of a path.
 */
PUBLIC struct inode *inode_dname(const char *path, const char **name)
{
	dev_t dev;                   /* Current device.     */
	ino_t ent;                   /* Directory entry.    */
	struct inode *i;             /* Working inode.      */
	const char *p;               /* Current path.       */
	struct superblock *sb;       /* Working superblock. */
	char filename[NAME_MAX + 1]; /* File name.          */
	struct mountingPoint * mp;

	p = path;
	
	/* Absolute path. */
	if (*p == '/')
	{
		i = curr_proc->root;

	}
	/* Relative path. */
	else if (*p != '\0'){
		i = curr_proc->pwd;
	}
		
	/* Empty path name. */
	else
	{
		curr_proc->errno = -EINVAL;
		return (NULL);
	}
	
	i->count++;
	inode_lock(i);
	
	p = break_path((*name) = p, filename);

	/* Failed to break path. */
	if (p == NULL)
		goto error0;
	
	/* Parse all file path. */
	while (*p != '\0')
	{
		/* Not a directory. */
		if (!S_ISDIR(i->mode))
		{
			curr_proc->errno = -ENOTDIR;
			goto error0;
		}

again:

		/* Permission denied. */
		if (!permission(i->mode, i->uid, i->gid, curr_proc, MAY_EXEC, 0))
		{
			curr_proc->errno = -EACCES;
			goto error0;
		}

		/* Root directory reached. */
		if ((curr_proc->root->num == i->num) && (!kstrcmp(filename, "..")))
		{
			do
			{
				p = break_path((*name) = p, filename);
				
				if (p == NULL)
					goto error0;
			
			} while (!kstrcmp(filename, ".."));
		}

		/*Search if we are in the root of a file system */
		if (((mp=is_root_fs(i->num))!=NULL&&(!kstrcmp(filename, ".."))))
		{
			inode_put(i);
			i=inode_get(mp->dev,mp->no_inode_mount);
		}
		
		ent = dir_search(i, filename);
			
		/* No such file or directory. */
		if (ent == INODE_NULL)
		{
			/* This path was really invalid. */
			if (*p != '\0')
			{			
				curr_proc->errno = -ENOENT;
				goto error0;	
			}
			
			/* Let someone else decide what to do. */
			break;
		}
			
		/* Cross mount point. */
		if ((i->num == INODE_ROOT) && (!kstrcmp(filename, "..")))
		{
			sb = i->sb;
			inode_put(i);
			i = sb->mp;
			inode_lock(i);
			i->count++;
			goto again;
		}
			
		dev = i->dev;	
		inode_put(i);
		i = inode_get(dev, ent);
		
		/*Mounting point */
		mp=belong_mounting_table(i->num);
		if (mp!=NULL){
			inode_put(i);  
			i=inode_get (mp->dev,mp->no_inode_root_fs);
		}		

		/* Failed to get inode. */
		if (i == NULL)
			return (NULL);
		
		p = break_path((*name) = p, filename);
		
		/* Failed to break path. */
		if (p == NULL)
			goto error0;
	}
	
	/* Special treatment for root directory. */
	if (kstrcmp(*name, "/"))
	{
		while (**name == '/')
			(*name)++;
	}

	return (i);

error0:
	inode_put(i);
	return (NULL);
}

/*
 * Converts a path name to inode.
 */
PUBLIC struct inode *inode_name(const char *pathname)
{
	dev_t dev;           /* Device number. */
	ino_t num;           /* Inode number.  */
	const char *name;    /* File name.     */
	struct inode *inode; /* Working inode. */
	struct mountingPoint *mp;
	
	inode = inode_dname(pathname, &name);
	
	/* Failed to get directory inode. */
	if (inode == NULL)
		return (NULL);
		
	/* Special treatment for the root directory. */
	if (!kstrcmp(name,"/"))
		num = curr_proc->root->num;	
	
	else{
		/*Search if we are in the root of a file system */
		if (((mp=is_root_fs(inode->num))!=NULL&&(!kstrcmp(name, ".."))))
		{
			inode_unlock(inode);
			inode=inode_get(mp->dev,mp->no_inode_mount);
		}
		num = dir_search(inode, name);
	}

	/* File not found. */
	if (num == INODE_NULL)
	{	
		inode_put(inode);
		curr_proc->errno = -ENOENT;
		return (NULL);
	}

	dev = inode->dev;	
	inode_put(inode);
	
	inode =inode_get(dev, num);
	/*Mounting point */
	mp=belong_mounting_table(inode->num);
	if (mp!=NULL){
		inode_unlock(inode);
		inode=inode_get (mp->dev,mp->no_inode_root_fs);
	}
	
	return inode;
}

/**
 * @brief initialize the mountable 
 * 
 * @details mark all the mount point of the mounting table to available
 *         
 */
PRIVATE void init_mountTable(void)
{
	kprintf("Initialisation of the mountTable\n");
	for (int i=0; i<NR_MOUNTING_POINT; i++)
		mountTable[i].free=1;
}

/**
 * @brief put the ROOTDEV in the mountable. Need to be done only once, at the initialisation
 */
PUBLIC struct inode * mountRoot ()
{
	mountTable[0].dev=ROOT_DEV;
	mountTable[0].fs= fileSystemTable[MINIX];
	mountTable[0].no_inode_root_fs=1;
	mountTable[0].no_inode_mount=-1;
	mountTable[0].free= 0;

	root = inode_get(ROOT_DEV, 1);
	
	/* Failed to read root inode. */
	if (root == NULL)
		kpanic("failed to read root inode");
	return root;
}

/**
 * @brief Initializes the inode table.
 * 
 * @details Initializes the in-core inode table by marking all in-core inodes
 *          as invalid and not locked.
 */
PUBLIC void inode_init(void)
{
	kprintf("fs: initializing inode cache");
	
	/* Initialize inodes. */
	for (unsigned i = 0; i < NR_INODES; i++)
	{
		inodes[i].count = 0;
		inodes[i].flags = ~(INODE_LOCKED | INODE_VALID);
		inodes[i].chain = NULL;
		inodes[i].free_next = ((i + 1) < NR_INODES) ? &inodes[i + 1] : NULL;
		inodes[i].hash_next = NULL;
		inodes[i].hash_prev = NULL;
	}
	
	/* Initialize inode cache. */
	free_inodes = &inodes[0];
	for (unsigned i = 0; i < HASHTAB_SIZE; i++)
		hashtab[i] = NULL;

	/*Initialize FileSystemTable*/
	init_minix();

	/*Initialize MountTable*/
	init_mountTable();
}
