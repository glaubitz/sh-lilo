/* geometry.c  -  Device and file geometry computation */

/* Copyright 1992-1998 Werner Almesberger. See file COPYING for details. */


#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <linux/fs.h>
#include <linux/fd.h>
#include <linux/hdreg.h>

#include "config.h"
#include "lilo.h"
#include "common.h"
#include "device.h"
#include "geometry.h"
#include "cfg.h"


#ifndef HDIO_GETGEO
#define HDIO_GETGEO HDIO_REQ
#endif


typedef struct _st_buf {
    struct _st_buf *next;
    struct stat st;
} ST_BUF;


DT_ENTRY *disktab = NULL;
int old_disktab = 0;


void geo_init(char *name)
{
    FILE *file;
    char line[MAX_LINE+1];
    char *here;
    DT_ENTRY *entry;
    int disk_section,items;

    if (name) {
	if ((file = fopen(name,"r")) == NULL)
	    die("open %s: %s",name,strerror(errno));
    }
    else if ((file = fopen(DFL_DISKTAB,"r")) == NULL) return;
    disk_section = !!disktab;
    while (fgets(line,MAX_LINE,file)) {
	here = strchr(line,'\n');
	if (here) *here = 0;
	here = strchr(line,'#');
	if (here) *here = 0;
	if (strspn(line," \t") != strlen(line)) {
	    entry = alloc_t(DT_ENTRY);
	    items = sscanf(line,"0x%x 0x%x %d %d %d %d",&entry->device,
	      &entry->bios,&entry->sectors,&entry->heads,&entry->cylinders,
	      &entry->start);
	    if (items == 5) entry->start = -1;
	    if (items < 5)
		die("Invalid line in %s:\n\"%s\"",name ? name : DFL_DISKTAB,
		  line);
	    entry->next = disktab;
	    disktab = entry;
	    if (disk_section) die("DISKTAB and DISK are mutually exclusive");
	    old_disktab = 1;
	}
    }
    (void) fclose(file);
}


void do_partition(void)
{
    DT_ENTRY *entry,*walk;
    struct stat st;
    char *partition,*start;

    entry = alloc_t(DT_ENTRY);
    *entry = *disktab;
    entry->start = -1;
    partition = cfg_get_strg(cf_partitions,"partition");
    if (stat(partition,&st) < 0) die("stat %s: %s",partition,strerror(errno));
    if (!S_ISBLK(st.st_mode) || ((st.st_rdev ^ disktab->device) & ~PART_MASK))
	die("%s is not a valid partition device",partition);
    entry->device = st.st_rdev;
    cfg_init(cf_partition);
    (void) cfg_parse(cf_partition);
    start = cfg_get_strg(cf_partition,"start");
    entry->start = start ? to_number(start) : -1;
    for (walk = disktab; walk; walk = walk->next)
	if (entry->device == walk->device)
	    die("Duplicate geometry definition for %s",partition);
    entry->next = disktab;
    disktab = entry;
    cfg_init(cf_partitions);
}


static int has_partitions(dev_t dev)
{
    return MAJOR(dev) == MAJOR_HD || MAJOR(dev) == MAJOR_IDE2 ||
      MAJOR(dev) == MAJOR_IDE3 || MAJOR(dev) == MAJOR_IDE4 ||
      MAJOR(dev) == MAJOR_IDE5 || MAJOR(dev) == MAJOR_IDE6 ||
      MAJOR(dev) == MAJOR_XT || MAJOR(dev) == MAJOR_SD ||
      MAJOR(dev) == MAJOR_ESDI || MAJOR(dev) == MAJOR_DAC960;
}


void do_disk(void)
{
    DT_ENTRY *entry,*walk;
    struct stat st;
    char *disk,*bios,*sectors,*heads,*cylinders;

    entry = alloc_t(DT_ENTRY);
    disk = cfg_get_strg(cf_options,"disk");
    if (stat(disk,&st) < 0) die("stat %s: %s",disk,strerror(errno));
    if (!S_ISBLK(st.st_mode) || ((MINOR(st.st_rdev) & PART_MASK) &&
      has_partitions(st.st_rdev)))
	die("RSN: %s is not a whole disk device",disk);
    entry->device = st.st_rdev;
    cfg_init(cf_disk);
    (void) cfg_parse(cf_disk);
    bios = cfg_get_strg(cf_disk,"bios");
    sectors = cfg_get_strg(cf_disk,"sectors");
    heads = cfg_get_strg(cf_disk,"heads");
    cylinders = cfg_get_strg(cf_disk,"cylinders");
    entry->bios = bios ? to_number(bios) : -1;
    if (!sectors && !heads) entry->sectors = entry->heads = -1;
    else if (!(sectors && heads))
	    die("Must specify SECTORS and HEADS together");
	else {
	    entry->sectors = to_number(sectors);
	    entry->heads = to_number(heads);
	}
    if (cfg_get_flag(cf_disk,"inaccessible")) {
	entry->heads = 0;
	if (cfg_get_strg(cf_disk,"bios") || cfg_get_strg(cf_disk,"sectors") ||
	  cfg_get_strg(cf_disk,"heads") || cfg_get_strg(cf_disk,"cylinders"))
	    die("No geometry variables allowed if INACCESSIBLE");
    }
    entry->cylinders = cylinders ? to_number(cylinders) : -1;
    entry->start = 0;
    for (walk = disktab; walk; walk = walk->next)
	if (entry->device == walk->device)
	    die("Duplicate geometry definition for %s",disk);
    entry->next = disktab;
    disktab = entry;
    cfg_init(cf_partitions);
    (void) cfg_parse(cf_partitions);
    cfg_unset(cf_options,"disk");
}


static int exists(const char *name)
{
    struct hd_geometry dummy;
    int fd,yes;
    char buff;

    if ((fd = open(name,O_RDWR)) < 0) return 0; /* was O_RDONLY */
    yes = read(fd,&buff,1) == 1 && ioctl(fd,HDIO_GETGEO,&dummy) >= 0;
    (void) close(fd);
    return yes;
}


#if 0

static int scan_last_dev(ST_BUF *next,char *parent,int major,int increment)
{
    DIR *dp;
    struct dirent *dir;
    char name[PATH_MAX+1];
    ST_BUF st,*walk;
    int max,this;

    st.next = next;
    max = 0;
    if ((dp = opendir(parent)) == NULL)
	die("opendir %s: %s",parent,strerror(errno));
    while ((dir = readdir(dp))) {
	sprintf(name,"%s/%s",parent,dir->d_name);
	if (stat(name,&st.st) >= 0) {
	    if (S_ISBLK(st.st.st_mode) && MAJOR(st.st.st_rdev) == major &&
	      (MINOR(st.st.st_rdev) & (increment-1)) == 0) {
		this = MINOR(st.st.st_rdev)/increment+1;
		if (this > max && exists(name)) max = this;
	    }
	    if (S_ISDIR(st.st.st_mode) && strcmp(dir->d_name,".") &&
	      strcmp(dir->d_name,"..")) {
		for (walk = next; walk; walk = walk->next)
		    if (stat_equal(&walk->st,&st.st)) break;
		if (!walk) {
		    this = scan_last_dev(&st,name,major,increment);
		    if (this > max) max = this;
		}
	    }
	}
    }
    (void) closedir(dp);
    return max;
}

#endif


static int last_dev(int major,int increment)
{
/*
 * In version 12 to 18, LILO only relied on scan_last_dev (or last_dev). This
 * obviously didn't work if entries in /dev were missing. Versions 18 and 19
 * added the probe loop, which seems to be okay, but which may probe for
 * invalid minor numbers. The IDE driver objects to that. Since last_dev is
 * only used to count IDE drives anyway, we try now only the first two devices
 * and forget about scan_last_dev.
 */
#if 0
    DEVICE dev;
    int minor;

    for (minor = 0; dev_open(&dev,(major << 8) | minor,-1); minor += increment)
	if (exists(dev.name)) dev_close(&dev);
        else {
	    dev_close(&dev);
	    return minor/increment;
	}
    return scan_last_dev(NULL,DEV_DIR,major,increment);
#else
    DEVICE dev;
    int devs;

    for (devs = 0; devs < 2 && dev_open(&dev,(major << 8) | (increment*devs),
      -1); devs++)
	if (exists(dev.name)) dev_close(&dev);
        else {
	    dev_close(&dev);
	    break;
	}
    return devs;
#endif
}


void geo_query_dev(GEOMETRY *geo,int device,int all)
{
    DEVICE dev;
    int fd,get_all;
    struct floppy_struct fdprm;
    struct hd_geometry hdprm;

    get_all = all || MAJOR(device) != MAJOR_FD;
    if (!MAJOR(device))
	die("Trying to map files from unnamed device 0x%04x (NFS ?)",device);
    if (device == MAJMIN_RAM)
	die("Trying to map files from your RAM disk. "
	  "Please check -r option or ROOT environment variable.");
    if (get_all) fd = dev_open(&dev,device,O_NOACCESS);
    else fd = -1; /* pacify GCC */
    switch (MAJOR(device)) {
	case MAJOR_FD:
	    geo->device = device & 3;
	    if (!get_all) {
		geo->heads = geo->cylinders = geo->sectors = 1;
		geo->start = 0;
		break;
	    }
	    if (ioctl(fd,FDGETPRM,&fdprm) < 0)
		die("geo_query_dev FDGETPRM (dev 0x%04x): %s",device,
		  strerror(errno));
	    geo->heads = fdprm.head;
	    geo->cylinders = fdprm.track;
	    geo->sectors = fdprm.sect;
	    geo->start = 0;
	    break;
	case MAJOR_HD:
	    /* fall through */
	case MAJOR_IDE2:
	    /* fall through */
	case MAJOR_IDE3:
	    /* fall through */
	case MAJOR_IDE4:
	    /* fall through */
	case MAJOR_IDE5:
	    /* fall through */
	case MAJOR_IDE6:
	    /* fall through */
	case MAJOR_ESDI:
	    /* fall through */
	case MAJOR_XT:
	    geo->device = 0x80+(MINOR(device) >> 6)+(MAJOR(device) == MAJOR_HD ?
	      0 : last_dev(MAJOR_HD,64));
	    if (ioctl(fd,HDIO_GETGEO,&hdprm) < 0)
		die("geo_query_dev HDIO_GETGEO (dev 0x%04x): %s",device,
		  strerror(errno));
	    geo->heads = hdprm.heads;
	    geo->cylinders = hdprm.cylinders;
	    geo->sectors = hdprm.sectors;
	    geo->start = hdprm.start;
	    break;
	case MAJOR_SD:
	    geo->device = 0x80+last_dev(MAJOR_HD,64)+(MINOR(device) >> 4);
	    if (ioctl(fd,HDIO_GETGEO,&hdprm) < 0)
		die("geo_query_dev HDIO_GETGEO (dev 0x%04x): %s",device,
		  strerror(errno));
	    if (all && !hdprm.sectors)
		die("HDIO_REQ not supported for your SCSI controller. Please "
		  "use a DISK section");
	    geo->heads = hdprm.heads;
	    geo->cylinders = hdprm.cylinders;
	    geo->sectors = hdprm.sectors;
	    geo->start = hdprm.start;
	    break;
	case MAJOR_DAC960:
	    geo->device = 0x80+last_dev(MAJOR_HD,64)+(MINOR(device) >> 3);
	    if (ioctl(fd,HDIO_GETGEO,&hdprm) < 0)
		die("geo_query_dev HDIO_GETGEO (dev 0x%04x): %s",device,
		  strerror(errno));
	    if (all && !hdprm.sectors)
		die("HDIO_REQ not supported for your DAC960 controller. "
		  "Please use a DISK section");
	    geo->heads = hdprm.heads;
	    geo->cylinders = hdprm.cylinders;
	    geo->sectors = hdprm.sectors;
	    geo->start = hdprm.start;
	    break;
	case COMPAQ_SMART2_MAJOR+0:
	case COMPAQ_SMART2_MAJOR+1:
	case COMPAQ_SMART2_MAJOR+2:
	case COMPAQ_SMART2_MAJOR+3:
	case COMPAQ_SMART2_MAJOR+4:
	case COMPAQ_SMART2_MAJOR+5:
	case COMPAQ_SMART2_MAJOR+6:
	case COMPAQ_SMART2_MAJOR+7:
	    geo->device = 0x80+last_dev(MAJOR_HD,64)+(MINOR(device) >> 4);
	    if (ioctl(fd,HDIO_GETGEO,&hdprm) < 0)
		die("geo_query_dev HDIO_GETGEO (dev 0x%04x): %s",device,
		  strerror(errno));
	    if (all && !hdprm.sectors)
		die("HDIO_REQ not supported for your Array controller. Please "
		  "use a DISK section");
	    geo->heads = hdprm.heads;
	    geo->cylinders = hdprm.cylinders;
	    geo->sectors = hdprm.sectors;
	    geo->start = hdprm.start;
	    break;

	default:
	    die("Sorry, don't know how to handle device 0x%04x",device);
    }
    if (get_all) dev_close(&dev);
}


int is_first(int device)
{
    DT_ENTRY *walk;

    for (walk = disktab; walk; walk = walk->next)
	if (walk->device == device) break;
    if (!walk && !old_disktab)
	for (walk = disktab; walk; walk = walk->next)
	    if (walk->device == (device & ~PART_MASK)) break;
    if (walk && !walk->heads)
	die("Device 0x%04X: Configured as inaccessible.\n",device);
    if (walk && walk->bios != -1) return !(walk->bios & 0x7f);
    switch (MAJOR(device)) {
	case MAJOR_FD:
	    return !(device & 3);
	case MAJOR_HD:
	    return !(MINOR(device) >> 6);
	case MAJOR_IDE2:
	    /* fall through */
	case MAJOR_IDE3:
	    /* fall through */
	case MAJOR_IDE4:
	    /* fall through */
	case MAJOR_IDE5:
	    /* fall through */
	case MAJOR_IDE6:
	    /* fall through */
	case MAJOR_ESDI:
	    /* fall through */
	case MAJOR_XT:
	    return MINOR(device) >> 6 ? 0 : !last_dev(MAJOR_HD,64);
	case MAJOR_SD:
	    return MINOR(device) >> 4 ? 0 : !last_dev(MAJOR_HD,64);
	case MAJOR_DAC960:
	    return MINOR(device) >> 3 ? 0 : !last_dev(MAJOR_HD,64);
	default:
	    return 1; /* user knows what (s)he's doing ... I hope */
    }
}


void geo_get(GEOMETRY *geo,int device,int user_device,int all)
{
    DT_ENTRY *walk;
    int inherited,keep_cyls;

    for (walk = disktab; walk; walk = walk->next)
	if (walk->device == device) break;
    inherited = !walk && !old_disktab;
    if (inherited)
	for (walk = disktab; walk; walk = walk->next)
	    if (walk->device == (device & ~PART_MASK)) break;
    if (walk && !walk->heads)
	die("Device 0x%04X: Configured as inaccessible.\n",device);
    keep_cyls = !walk || walk->bios == -1 || walk->heads == -1 ||
      walk->sectors == -1 || inherited || walk->start == -1;
    if (keep_cyls) {
	geo_query_dev(geo,device,all);
	if (!nowarn && (geo->device & 0x7f) >= BIOS_MAX_DEVS &&
	  user_device == -1 && (!walk || walk->bios == -1))
	    fprintf(stderr,"Warning: BIOS drive 0x%02x may not be accessible\n",
	      geo->device);
    }
    if (walk) {
	if (walk->bios != -1) geo->device = walk->bios;
	if (walk->heads != -1) geo->heads = walk->heads;
	if (walk->cylinders != -1 || !keep_cyls)
	    geo->cylinders = walk->cylinders;
	if (walk->sectors != -1) geo->sectors = walk->sectors;
	if (walk->start != -1 && !inherited) geo->start = walk->start;
    }
    if (user_device != -1) geo->device = user_device;
    if (!all) {
	if (verbose > 2)
	    printf("Device 0x%04x: BIOS drive 0x%02x, no geometry.\n",device,
	      geo->device);
	return;
    }
    if (!geo->heads || !geo->cylinders || !geo->sectors)
	die("Device 0x%04X: Got bad geometry %d/%d/%d\n",device,
	  geo->sectors,geo->heads,geo->cylinders);
    if (geo->heads > BIOS_MAX_HEADS)
	die("Device 0x%04X: Maximum number of heads is %d, not %d\n",device,
	  BIOS_MAX_HEADS,geo->heads);
    if (geo->sectors > BIOS_MAX_SECS)
	die("Device 0x%04X: Maximum number of sectors is %d, not %d\n",
	  device,BIOS_MAX_SECS,geo->sectors);
    if ((geo->start+geo->sectors-1)/geo->heads/geo->sectors >= BIOS_MAX_CYLS &&
      !nowarn)
	fprintf(stderr,"Warning: device 0x%04x exceeds %d cylinder limit\n",
	  device,BIOS_MAX_CYLS);
    if (verbose > 2) {
	printf("Device 0x%04x: BIOS drive 0x%02x, %d heads, %d cylinders,\n",
	  device,geo->device,geo->heads,geo->cylinders == -1 ? BIOS_MAX_CYLS :
	  geo->cylinders);
	printf("%15s%d sectors. Partition offset: %d sectors.\n","",
	  geo->sectors,geo->start);
    }
}


int geo_open(GEOMETRY *geo,char *name,int flags)
{
    char *here;
    int user_dev,block_size;
    struct stat st;

    if ((here = strrchr(name,':')) == NULL) user_dev = -1;
    else {
	fprintf(stderr,":BIOS syntax is no longer supported. Please use a "
	  "DISK section\n");
	*here++ = 0;
	user_dev = to_number(here);
    }
    if ((geo->fd = open(name,flags)) < 0)
	die("open %s: %s",name,strerror(errno));
    if (fstat(geo->fd,&st) < 0) die("fstat %s: %s",name,strerror(errno));
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode))
	die("%s: neither a reg. file nor a block dev.",name);
    geo_get(geo,S_ISREG(st.st_mode) ? st.st_dev : st.st_rdev,user_dev,1);
    geo->file = S_ISREG(st.st_mode);
    geo->boot = 0;
#ifndef FIGETBSZ
    geo->spb = 2;
#else
    if (!geo->file) geo->spb = 2;
    else {
	if (ioctl(geo->fd,FIGETBSZ,&block_size) < 0) {
	    fprintf(stderr,"FIGETBSZ %s: %s\n",name,strerror(errno));
	    geo->spb = 2;
	}
	else {
	    if (!block_size || (block_size & (SECTOR_SIZE-1)))
		die("Incompatible block size: %d\n",block_size);
	    geo->spb = block_size/SECTOR_SIZE;
	}
    }
#endif
    return geo->fd;
}


int geo_open_boot(GEOMETRY *geo,char *name)
{
    struct stat st;
    dev_t dev;

    if (stat(name,&st) < 0) die("stat %s: %s",name,strerror(errno));
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode))
	die("%s: neither a reg. file nor a block dev.",name);
    dev = S_ISREG(st.st_mode) ? st.st_dev : st.st_rdev;
    if (MAJOR(dev) == MAJOR_FD) geo->fd = 0;
    else if ((geo->fd = open(name,O_NOACCESS)) < 0)
	    die("open %s: %s",name,strerror(errno));
    geo_get(geo,dev,-1,0);
    geo->file = S_ISREG(st.st_mode);
    geo->boot = 1;
    geo->spb = 1;
    return geo->fd;
}


void geo_close(GEOMETRY *geo)
{
    if (geo->fd) (void) close(geo->fd);
}


#ifndef FIBMAP
#define FIBMAP BMAP_IOCTL
#endif


int geo_comp_addr(GEOMETRY *geo,int offset,SECTOR_ADDR *addr)
{
    int block,sector;

    if (geo->boot && offset >= SECTOR_SIZE)
	die("Internal error: sector > 0 after geo_open_boot");
    block = offset/geo->spb/SECTOR_SIZE;
    if (geo->file) {
	if (ioctl(geo->fd,FIBMAP,&block) < 0) pdie("ioctl FIBMAP");
	if (!block) return 0;
    }
    sector = block*geo->spb+((offset/SECTOR_SIZE) % geo->spb);
    sector += geo->start;
    if (linear) {
	addr->device = geo->device | LINEAR_FLAG;
	addr->sector = sector & 0xff;
	addr->track = (sector >> 8) & 0xff;
	addr->head = sector >> 16;
	if (verbose > 4)
	    printf("fd %d: offset %d -> linear %d\n",geo->fd,offset,sector);
    }
    else {
	addr->device = geo->device;
	addr->sector = (sector % geo->sectors)+1;
	sector /= geo->sectors;
	addr->head = sector % geo->heads;
	sector /= geo->heads;
	if (sector >= BIOS_MAX_CYLS)
	    die("geo_comp_addr: Cylinder number is too big (%d > %d)",sector,
	      BIOS_MAX_CYLS-1);
	if (sector >= geo->cylinders && geo->cylinders != -1)
	    die("geo_comp_addr: Cylinder %d beyond end of media (%d)",sector,
	      geo->cylinders);
	if (verbose > 4)
	    printf("fd %d: offset %d -> dev %d, head %d, track %d, sector %d\n",
	      geo->fd,offset,addr->device,addr->head,sector,addr->sector);
	addr->track = sector & 255;
	addr->sector |= (sector >> 8) << 6;
    }
    addr->num_sect = 1;
    return 1;
}


int geo_find(GEOMETRY *geo,SECTOR_ADDR addr)
{
    SECTOR_ADDR here;
    struct stat st;
    int i;

    if (fstat(geo->fd,&st) < 0) return 0;
    geo_get(geo,st.st_dev,-1,1);
    for (i = 0; i < (st.st_size+SECTOR_SIZE-1)/SECTOR_SIZE; i++)
	if (geo_comp_addr(geo,i*SECTOR_SIZE,&here))
	    if (here.sector == addr.sector && here.track == addr.track &&
	      here.device == addr.device && here.head == addr.head)
		if (lseek(geo->fd,i*SECTOR_SIZE,0) < 0) return 0;
		else return 1;
    return 1;
}
