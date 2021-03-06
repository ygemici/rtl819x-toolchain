/* 
   Unix SMB/CIFS implementation.
   Inter-process communication and named pipe handling
   Copyright (C) Andrew Tridgell 1992-1998
   Copyright (C) Jeremy Allison 2007.

   SMB Version handling
   Copyright (C) John H Terpstra 1995-1998
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
   */
/*
   This file handles the named pipe and mailslot calls
   in the SMBtrans protocol
   */

#include "includes.h"

extern struct current_user current_user;
extern userdom_struct current_user_info;

#ifdef CHECK_TYPES
#undef CHECK_TYPES
#endif
#define CHECK_TYPES 0

#define NERR_Success 0
#define NERR_badpass 86
#define NERR_notsupported 50

#define NERR_BASE (2100)
#define NERR_BufTooSmall (NERR_BASE+23)
#define NERR_JobNotFound (NERR_BASE+51)
#define NERR_DestNotFound (NERR_BASE+52)

#define ACCESS_READ 0x01
#define ACCESS_WRITE 0x02
#define ACCESS_CREATE 0x04

#define SHPWLEN 8		/* share password length */

/* Limit size of ipc replies */

static char *smb_realloc_limit(void *ptr, size_t size)
{
	char *val;

	size = MAX((size),4*1024);
	val = (char *)SMB_REALLOC(ptr,size);
	if (val) {
		memset(val,'\0',size);
	}
	return val;
}

static bool api_Unsupported(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt, int mprcnt,
				char **rdata, char **rparam,
				int *rdata_len, int *rparam_len);

static bool api_TooSmall(connection_struct *conn, uint16 vuid, char *param, char *data,
			 int mdrcnt, int mprcnt,
			 char **rdata, char **rparam,
			 int *rdata_len, int *rparam_len);


static int CopyExpanded(connection_struct *conn,
			int snum, char **dst, char *src, int *p_space_remaining)
{
	TALLOC_CTX *ctx = talloc_tos();
	char *buf = NULL;
	int l;

	if (!src || !dst || !p_space_remaining || !(*dst) ||
			*p_space_remaining <= 0) {
		return 0;
	}

	buf = talloc_strdup(ctx, src);
	if (!buf) {
		*p_space_remaining = 0;
		return 0;
	}
	buf = talloc_string_sub(ctx, buf,"%S",lp_servicename(snum));
	if (!buf) {
		*p_space_remaining = 0;
		return 0;
	}
	buf = talloc_sub_advanced(ctx,
				lp_servicename(SNUM(conn)),
				conn->user,
				conn->connectpath,
				conn->gid,
				get_current_username(),
				current_user_info.domain,
				buf);
	if (!buf) {
		*p_space_remaining = 0;
		return 0;
	}
	l = push_ascii(*dst,buf,*p_space_remaining, STR_TERMINATE);
	if (l == -1) {
		return 0;
	}
	(*dst) += l;
	(*p_space_remaining) -= l;
	return l;
}

static int CopyAndAdvance(char **dst, char *src, int *n)
{
	int l;
	if (!src || !dst || !n || !(*dst)) {
		return 0;
	}
	l = push_ascii(*dst,src,*n, STR_TERMINATE);
	if (l == -1) {
		return 0;
	}
	(*dst) += l;
	(*n) -= l;
	return l;
}

static int StrlenExpanded(connection_struct *conn, int snum, char *s)
{
	TALLOC_CTX *ctx = talloc_tos();
	char *buf = NULL;
	if (!s) {
		return 0;
	}
	buf = talloc_strdup(ctx,s);
	if (!buf) {
		return 0;
	}
	buf = talloc_string_sub(ctx,buf,"%S",lp_servicename(snum));
	if (!buf) {
		return 0;
	}
	buf = talloc_sub_advanced(ctx,
				lp_servicename(SNUM(conn)),
				conn->user,
				conn->connectpath,
				conn->gid,
				get_current_username(),
				current_user_info.domain,
				buf);
	if (!buf) {
		return 0;
	}
	return strlen(buf) + 1;
}

static char *Expand(connection_struct *conn, int snum, char *s)
{
	TALLOC_CTX *ctx = talloc_tos();
	char *buf = NULL;

	if (!s) {
		return NULL;
	}
	buf = talloc_strdup(ctx,s);
	if (!buf) {
		return 0;
	}
	buf = talloc_string_sub(ctx,buf,"%S",lp_servicename(snum));
	if (!buf) {
		return 0;
	}
	return talloc_sub_advanced(ctx,
				lp_servicename(SNUM(conn)),
				conn->user,
				conn->connectpath,
				conn->gid,
				get_current_username(),
				current_user_info.domain,
				buf);
}

/*******************************************************************
 Check a API string for validity when we only need to check the prefix.
******************************************************************/

static bool prefix_ok(const char *str, const char *prefix)
{
	return(strncmp(str,prefix,strlen(prefix)) == 0);
}

struct pack_desc {
	const char *format;	    /* formatstring for structure */
	const char *subformat;  /* subformat for structure */
	char *base;	    /* baseaddress of buffer */
	int buflen;	   /* remaining size for fixed part; on init: length of base */
	int subcount;	    /* count of substructures */
	char *structbuf;  /* pointer into buffer for remaining fixed part */
	int stringlen;    /* remaining size for variable part */		
	char *stringbuf;  /* pointer into buffer for remaining variable part */
	int neededlen;    /* total needed size */
	int usedlen;	    /* total used size (usedlen <= neededlen and usedlen <= buflen) */
	const char *curpos;	    /* current position; pointer into format or subformat */
	int errcode;
};

static int get_counter(const char **p)
{
	int i, n;
	if (!p || !(*p)) {
		return 1;
	}
	if (!isdigit((int)**p)) {
		return 1;
	}
	for (n = 0;;) {
		i = **p;
		if (isdigit(i)) {
			n = 10 * n + (i - '0');
		} else {
			return n;
		}
		(*p)++;
	}
}

static int getlen(const char *p)
{
	int n = 0;
	if (!p) {
		return 0;
	}

	while (*p) {
		switch( *p++ ) {
		case 'W':			/* word (2 byte) */
			n += 2;
			break;
		case 'K':			/* status word? (2 byte) */
			n += 2;
			break;
		case 'N':			/* count of substructures (word) at end */
			n += 2;
			break;
		case 'D':			/* double word (4 byte) */
		case 'z':			/* offset to zero terminated string (4 byte) */
		case 'l':			/* offset to user data (4 byte) */
			n += 4;
			break;
		case 'b':			/* offset to data (with counter) (4 byte) */
			n += 4;
			get_counter(&p);
			break;
		case 'B':			/* byte (with optional counter) */
			n += get_counter(&p);
			break;
		}
	}
	return n;
}

static bool init_package(struct pack_desc *p, int count, int subcount)
{
	int n = p->buflen;
	int i;

	if (!p->format || !p->base) {
		return False;
	}

	i = count * getlen(p->format);
	if (p->subformat) {
		i += subcount * getlen(p->subformat);
	}
	p->structbuf = p->base;
	p->neededlen = 0;
	p->usedlen = 0;
	p->subcount = 0;
	p->curpos = p->format;
	if (i > n) {
		p->neededlen = i;
		i = n = 0;
#if 0
		/*
		 * This is the old error code we used. Aparently
		 * WinNT/2k systems return ERRbuftoosmall (2123) and
		 * OS/2 needs this. I'm leaving this here so we can revert
		 * if needed. JRA.
		 */
		p->errcode = ERRmoredata;
#else
		p->errcode = ERRbuftoosmall;
#endif
	} else {
		p->errcode = NERR_Success;
	}
	p->buflen = i;
	n -= i;
	p->stringbuf = p->base + i;
	p->stringlen = n;
	return (p->errcode == NERR_Success);
}

static int package(struct pack_desc *p, ...)
{
	va_list args;
	int needed=0, stringneeded;
	const char *str=NULL;
	int is_string=0, stringused;
	int32 temp;

	va_start(args,p);

	if (!*p->curpos) {
		if (!p->subcount) {
			p->curpos = p->format;
		} else {
			p->curpos = p->subformat;
			p->subcount--;
		}
	}
#if CHECK_TYPES
	str = va_arg(args,char*);
	SMB_ASSERT(strncmp(str,p->curpos,strlen(str)) == 0);
#endif
	stringneeded = -1;

	if (!p->curpos) {
		va_end(args);
		return 0;
	}

	switch( *p->curpos++ ) {
		case 'W':			/* word (2 byte) */
			needed = 2;
			temp = va_arg(args,int);
			if (p->buflen >= needed) {
				SSVAL(p->structbuf,0,temp);
			}
			break;
		case 'K':			/* status word? (2 byte) */
			needed = 2;
			temp = va_arg(args,int);
			if (p->buflen >= needed) {
				SSVAL(p->structbuf,0,temp);
			}
			break;
		case 'N':			/* count of substructures (word) at end */
			needed = 2;
			p->subcount = va_arg(args,int);
			if (p->buflen >= needed) {
				SSVAL(p->structbuf,0,p->subcount);
			}
			break;
		case 'D':			/* double word (4 byte) */
			needed = 4;
			temp = va_arg(args,int);
			if (p->buflen >= needed) {
				SIVAL(p->structbuf,0,temp);
			}
			break;
		case 'B':			/* byte (with optional counter) */
			needed = get_counter(&p->curpos);
			{
				char *s = va_arg(args,char*);
				if (p->buflen >= needed) {
					StrnCpy(p->structbuf,s?s:"",needed-1);
				}
			}
			break;
		case 'z':			/* offset to zero terminated string (4 byte) */
			str = va_arg(args,char*);
			stringneeded = (str ? strlen(str)+1 : 0);
			is_string = 1;
			break;
		case 'l':			/* offset to user data (4 byte) */
			str = va_arg(args,char*);
			stringneeded = va_arg(args,int);
			is_string = 0;
			break;
		case 'b':			/* offset to data (with counter) (4 byte) */
			str = va_arg(args,char*);
			stringneeded = get_counter(&p->curpos);
			is_string = 0;
			break;
	}

	va_end(args);
	if (stringneeded >= 0) {
		needed = 4;
		if (p->buflen >= needed) {
			stringused = stringneeded;
			if (stringused > p->stringlen) {
				stringused = (is_string ? p->stringlen : 0);
				if (p->errcode == NERR_Success) {
					p->errcode = ERRmoredata;
				}
			}
			if (!stringused) {
				SIVAL(p->structbuf,0,0);
			} else {
				SIVAL(p->structbuf,0,PTR_DIFF(p->stringbuf,p->base));
				memcpy(p->stringbuf,str?str:"",stringused);
				if (is_string) {
					p->stringbuf[stringused-1] = '\0';
				}
				p->stringbuf += stringused;
				p->stringlen -= stringused;
				p->usedlen += stringused;
			}
		}
		p->neededlen += stringneeded;
	}

	p->neededlen += needed;
	if (p->buflen >= needed) {
		p->structbuf += needed;
		p->buflen -= needed;
		p->usedlen += needed;
	} else {
		if (p->errcode == NERR_Success) {
			p->errcode = ERRmoredata;
		}
	}
	return 1;
}

#if CHECK_TYPES
#define PACK(desc,t,v) package(desc,t,v,0,0,0,0)
#define PACKl(desc,t,v,l) package(desc,t,v,l,0,0,0,0)
#else
#define PACK(desc,t,v) package(desc,v)
#define PACKl(desc,t,v,l) package(desc,v,l)
#endif

static void PACKI(struct pack_desc* desc, const char *t,int v)
{
	PACK(desc,t,v);
}

static void PACKS(struct pack_desc* desc,const char *t,const char *v)
{
	PACK(desc,t,v);
}

/****************************************************************************
 Get a print queue.
****************************************************************************/

static void PackDriverData(struct pack_desc* desc)
{
	char drivdata[4+4+32];
	SIVAL(drivdata,0,sizeof drivdata); /* cb */
	SIVAL(drivdata,4,1000);	/* lVersion */
	memset(drivdata+8,0,32);	/* szDeviceName */
	push_ascii(drivdata+8,"NULL",32, STR_TERMINATE);
	PACKl(desc,"l",drivdata,sizeof drivdata); /* pDriverData */
}

#ifndef AVM_NO_PRINTING
static int check_printq_info(struct pack_desc* desc,
				unsigned int uLevel, char *id1, char *id2)
{
	desc->subformat = NULL;
	switch( uLevel ) {
		case 0:
			desc->format = "B13";
			break;
		case 1:
			desc->format = "B13BWWWzzzzzWW";
			break;
		case 2:
			desc->format = "B13BWWWzzzzzWN";
			desc->subformat = "WB21BB16B10zWWzDDz";
			break;
		case 3:
			desc->format = "zWWWWzzzzWWzzl";
			break;
		case 4:
			desc->format = "zWWWWzzzzWNzzl";
			desc->subformat = "WWzWWDDzz";
			break;
		case 5:
			desc->format = "z";
			break;
		case 51:
			desc->format = "K";
			break;
		case 52:
			desc->format = "WzzzzzzzzN";
			desc->subformat = "z";
			break;
		default:
			DEBUG(0,("check_printq_info: invalid level %d\n",
				uLevel ));
			return False;
	}
	if (id1 == NULL || strcmp(desc->format,id1) != 0) {
		DEBUG(0,("check_printq_info: invalid format %s\n",
			id1 ? id1 : "<NULL>" ));
		return False;
	}
	if (desc->subformat && (id2 == NULL || strcmp(desc->subformat,id2) != 0)) {
		DEBUG(0,("check_printq_info: invalid subformat %s\n",
			id2 ? id2 : "<NULL>" ));
		return False;
	}
	return True;
}


#define RAP_JOB_STATUS_QUEUED 0
#define RAP_JOB_STATUS_PAUSED 1
#define RAP_JOB_STATUS_SPOOLING 2
#define RAP_JOB_STATUS_PRINTING 3
#define RAP_JOB_STATUS_PRINTED 4

#define RAP_QUEUE_STATUS_PAUSED 1
#define RAP_QUEUE_STATUS_ERROR 2

/* turn a print job status into a on the wire status 
*/
static int printj_status(int v)
{
	switch (v) {
	case LPQ_QUEUED:
		return RAP_JOB_STATUS_QUEUED;
	case LPQ_PAUSED:
		return RAP_JOB_STATUS_PAUSED;
	case LPQ_SPOOLING:
		return RAP_JOB_STATUS_SPOOLING;
	case LPQ_PRINTING:
		return RAP_JOB_STATUS_PRINTING;
	}
	return 0;
}

/* turn a print queue status into a on the wire status 
*/
static int printq_status(int v)
{
	switch (v) {
	case LPQ_QUEUED:
		return 0;
	case LPQ_PAUSED:
		return RAP_QUEUE_STATUS_PAUSED;
	}
	return RAP_QUEUE_STATUS_ERROR;
}

static void fill_printjob_info(connection_struct *conn, int snum, int uLevel,
			       struct pack_desc *desc,
			       print_queue_struct *queue, int n)
{
	time_t t = queue->time;

	/* the client expects localtime */
	t -= get_time_zone(t);

	PACKI(desc,"W",pjobid_to_rap(lp_const_servicename(snum),queue->job)); /* uJobId */
	if (uLevel == 1) {
		PACKS(desc,"B21",queue->fs_user); /* szUserName */
		PACKS(desc,"B","");		/* pad */
		PACKS(desc,"B16","");	/* szNotifyName */
		PACKS(desc,"B10","PM_Q_RAW"); /* szDataType */
		PACKS(desc,"z","");		/* pszParms */
		PACKI(desc,"W",n+1);		/* uPosition */
		PACKI(desc,"W",printj_status(queue->status)); /* fsStatus */
		PACKS(desc,"z","");		/* pszStatus */
		PACKI(desc,"D",t); /* ulSubmitted */
		PACKI(desc,"D",queue->size); /* ulSize */
		PACKS(desc,"z",queue->fs_file); /* pszComment */
	}
	if (uLevel == 2 || uLevel == 3 || uLevel == 4) {
		PACKI(desc,"W",queue->priority);		/* uPriority */
		PACKS(desc,"z",queue->fs_user); /* pszUserName */
		PACKI(desc,"W",n+1);		/* uPosition */
		PACKI(desc,"W",printj_status(queue->status)); /* fsStatus */
		PACKI(desc,"D",t); /* ulSubmitted */
		PACKI(desc,"D",queue->size); /* ulSize */
		PACKS(desc,"z","Samba");	/* pszComment */
		PACKS(desc,"z",queue->fs_file); /* pszDocument */
		if (uLevel == 3) {
			PACKS(desc,"z","");	/* pszNotifyName */
			PACKS(desc,"z","PM_Q_RAW"); /* pszDataType */
			PACKS(desc,"z","");	/* pszParms */
			PACKS(desc,"z","");	/* pszStatus */
			PACKS(desc,"z",SERVICE(snum)); /* pszQueue */
			PACKS(desc,"z","lpd");	/* pszQProcName */
			PACKS(desc,"z","");	/* pszQProcParms */
			PACKS(desc,"z","NULL"); /* pszDriverName */
			PackDriverData(desc);	/* pDriverData */
			PACKS(desc,"z","");	/* pszPrinterName */
		} else if (uLevel == 4) {   /* OS2 */
			PACKS(desc,"z","");       /* pszSpoolFileName  */
			PACKS(desc,"z","");       /* pszPortName       */
			PACKS(desc,"z","");       /* pszStatus         */
			PACKI(desc,"D",0);        /* ulPagesSpooled    */
			PACKI(desc,"D",0);        /* ulPagesSent       */
			PACKI(desc,"D",0);        /* ulPagesPrinted    */
			PACKI(desc,"D",0);        /* ulTimePrinted     */
			PACKI(desc,"D",0);        /* ulExtendJobStatus */
			PACKI(desc,"D",0);        /* ulStartPage       */
			PACKI(desc,"D",0);        /* ulEndPage         */
		}
	}
}

/********************************************************************
 Return a driver name given an snum.
 Returns True if from tdb, False otherwise.
 ********************************************************************/

static bool get_driver_name(int snum, char **pp_drivername)
{
	NT_PRINTER_INFO_LEVEL *info = NULL;
	bool in_tdb = false;

	get_a_printer (NULL, &info, 2, lp_servicename(snum));
	if (info != NULL) {
		*pp_drivername = talloc_strdup(talloc_tos(),
					info->info_2->drivername);
		in_tdb = true;
		free_a_printer(&info, 2);
		if (!*pp_drivername) {
			return false;
		}
	}

	return in_tdb;
}

/********************************************************************
 Respond to the DosPrintQInfo command with a level of 52
 This is used to get printer driver information for Win9x clients
 ********************************************************************/
static void fill_printq_info_52(connection_struct *conn, int snum, 
				struct pack_desc* desc,	int count )
{
	int 				i;
	fstring 			location;
	NT_PRINTER_DRIVER_INFO_LEVEL 	driver;
	NT_PRINTER_INFO_LEVEL 		*printer = NULL;

	ZERO_STRUCT(driver);

	if ( !W_ERROR_IS_OK(get_a_printer( NULL, &printer, 2, lp_servicename(snum))) ) {
		DEBUG(3,("fill_printq_info_52: Failed to lookup printer [%s]\n", 
			lp_servicename(snum)));
		goto err;
	}

	if ( !W_ERROR_IS_OK(get_a_printer_driver(&driver, 3, printer->info_2->drivername, 
		"Windows 4.0", 0)) )
	{
		DEBUG(3,("fill_printq_info_52: Failed to lookup driver [%s]\n", 
			printer->info_2->drivername));
		goto err;
	}

	trim_string(driver.info_3->driverpath, "\\print$\\WIN40\\0\\", 0);
	trim_string(driver.info_3->datafile, "\\print$\\WIN40\\0\\", 0);
	trim_string(driver.info_3->helpfile, "\\print$\\WIN40\\0\\", 0);

	PACKI(desc, "W", 0x0400);                     /* don't know */
	PACKS(desc, "z", driver.info_3->name);        /* long printer name */
	PACKS(desc, "z", driver.info_3->driverpath);  /* Driverfile Name */
	PACKS(desc, "z", driver.info_3->datafile);    /* Datafile name */
	PACKS(desc, "z", driver.info_3->monitorname); /* language monitor */

	fstrcpy(location, "\\\\%L\\print$\\WIN40\\0");
	standard_sub_basic( "", "", location, sizeof(location)-1 );
	PACKS(desc,"z", location);                          /* share to retrieve files */

	PACKS(desc,"z", driver.info_3->defaultdatatype);    /* default data type */
	PACKS(desc,"z", driver.info_3->helpfile);           /* helpfile name */
	PACKS(desc,"z", driver.info_3->driverpath);               /* driver name */

	DEBUG(3,("Printer Driver Name: %s:\n",driver.info_3->name));
	DEBUG(3,("Driver: %s:\n",driver.info_3->driverpath));
	DEBUG(3,("Data File: %s:\n",driver.info_3->datafile));
	DEBUG(3,("Language Monitor: %s:\n",driver.info_3->monitorname));
	DEBUG(3,("Driver Location: %s:\n",location));
	DEBUG(3,("Data Type: %s:\n",driver.info_3->defaultdatatype));
	DEBUG(3,("Help File: %s:\n",driver.info_3->helpfile));
	PACKI(desc,"N",count);                     /* number of files to copy */

	for ( i=0; i<count && driver.info_3->dependentfiles && *driver.info_3->dependentfiles[i]; i++) 
	{
		trim_string(driver.info_3->dependentfiles[i], "\\print$\\WIN40\\0\\", 0);
		PACKS(desc,"z",driver.info_3->dependentfiles[i]);         /* driver files to copy */
		DEBUG(3,("Dependent File: %s:\n",driver.info_3->dependentfiles[i]));
	}

	/* sanity check */
	if ( i != count )
		DEBUG(3,("fill_printq_info_52: file count specified by client [%d] != number of dependent files [%i]\n",
			count, i));

	DEBUG(3,("fill_printq_info on <%s> gave %d entries\n", SERVICE(snum),i));

        desc->errcode=NERR_Success;
	goto done;

err:
	DEBUG(3,("fill_printq_info: Can't supply driver files\n"));
	desc->errcode=NERR_notsupported;

done:
	if ( printer )
		free_a_printer( &printer, 2 );

	if ( driver.info_3 )
		free_a_printer_driver( driver, 3 );
}


static void fill_printq_info(connection_struct *conn, int snum, int uLevel,
 			     struct pack_desc* desc,
 			     int count, print_queue_struct* queue,
 			     print_status_struct* status)
{
	switch (uLevel) {
	case 1:
	case 2:
		PACKS(desc,"B13",SERVICE(snum));
		break;
	case 3:
	case 4:
	case 5:
		PACKS(desc,"z",Expand(conn,snum,SERVICE(snum)));
		break;
	case 51:
		PACKI(desc,"K",printq_status(status->status));
		break;
	}

	if (uLevel == 1 || uLevel == 2) {
		PACKS(desc,"B","");		/* alignment */
		PACKI(desc,"W",5);		/* priority */
		PACKI(desc,"W",0);		/* start time */
		PACKI(desc,"W",0);		/* until time */
		PACKS(desc,"z","");		/* pSepFile */
		PACKS(desc,"z","lpd");	/* pPrProc */
		PACKS(desc,"z",SERVICE(snum)); /* pDestinations */
		PACKS(desc,"z","");		/* pParms */
		if (snum < 0) {
			PACKS(desc,"z","UNKNOWN PRINTER");
			PACKI(desc,"W",LPSTAT_ERROR);
		}
		else if (!status || !status->message[0]) {
			PACKS(desc,"z",Expand(conn,snum,lp_comment(snum)));
			PACKI(desc,"W",LPSTAT_OK); /* status */
		} else {
			PACKS(desc,"z",status->message);
			PACKI(desc,"W",printq_status(status->status)); /* status */
		}
		PACKI(desc,(uLevel == 1 ? "W" : "N"),count);
	}

	if (uLevel == 3 || uLevel == 4) {
		char *drivername = NULL;

		PACKI(desc,"W",5);		/* uPriority */
		PACKI(desc,"W",0);		/* uStarttime */
		PACKI(desc,"W",0);		/* uUntiltime */
		PACKI(desc,"W",5);		/* pad1 */
		PACKS(desc,"z","");		/* pszSepFile */
		PACKS(desc,"z","WinPrint");	/* pszPrProc */
		PACKS(desc,"z",NULL);		/* pszParms */
		PACKS(desc,"z",NULL);		/* pszComment - don't ask.... JRA */
		/* "don't ask" that it's done this way to fix corrupted
		   Win9X/ME printer comments. */
		if (!status) {
			PACKI(desc,"W",LPSTAT_OK); /* fsStatus */
		} else {
			PACKI(desc,"W",printq_status(status->status)); /* fsStatus */
		}
		PACKI(desc,(uLevel == 3 ? "W" : "N"),count);	/* cJobs */
		PACKS(desc,"z",SERVICE(snum)); /* pszPrinters */
		get_driver_name(snum,&drivername);
		if (!drivername) {
			return;
		}
		PACKS(desc,"z",drivername);		/* pszDriverName */
		PackDriverData(desc);	/* pDriverData */
	}

	if (uLevel == 2 || uLevel == 4) {
		int i;
		for (i=0;i<count;i++)
			fill_printjob_info(conn,snum,uLevel == 2 ? 1 : 2,desc,&queue[i],i);
	}

	if (uLevel==52)
		fill_printq_info_52( conn, snum, desc, count );
}

/* This function returns the number of files for a given driver */
static int get_printerdrivernumber(int snum)
{
	int 				result = 0;
	NT_PRINTER_DRIVER_INFO_LEVEL 	driver;
	NT_PRINTER_INFO_LEVEL 		*printer = NULL;

	ZERO_STRUCT(driver);

	if ( !W_ERROR_IS_OK(get_a_printer( NULL, &printer, 2, lp_servicename(snum))) ) {
		DEBUG(3,("get_printerdrivernumber: Failed to lookup printer [%s]\n", 
			lp_servicename(snum)));
		goto done;
	}

	if ( !W_ERROR_IS_OK(get_a_printer_driver(&driver, 3, printer->info_2->drivername, 
		"Windows 4.0", 0)) )
	{
		DEBUG(3,("get_printerdrivernumber: Failed to lookup driver [%s]\n", 
			printer->info_2->drivername));
		goto done;
	}

	/* count the number of files */
	while ( driver.info_3->dependentfiles && *driver.info_3->dependentfiles[result] )
			result++;
			\
 done:
	if ( printer )
		free_a_printer( &printer, 2 );

	if ( driver.info_3 )
		free_a_printer_driver( driver, 3 );

	return result;
}
#endif

static bool api_DosPrintQGetInfo(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	char *QueueName = p;
	unsigned int uLevel;
	int count=0;
	int snum;
	char *str3;
	struct pack_desc desc;
	print_queue_struct *queue=NULL;
	print_status_struct status;
	char* tmpdata=NULL;

	if (!str1 || !str2 || !p) {
		return False;
	}
	memset((char *)&status,'\0',sizeof(status));
	memset((char *)&desc,'\0',sizeof(desc));

	p = skip_string(param,tpscnt,p);
	if (!p) {
		return False;
	}
	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	str3 = get_safe_str_ptr(param,tpscnt,p,4);
	/* str3 may be null here and is checked in check_printq_info(). */

	/* remove any trailing username */
	if ((p = strchr_m(QueueName,'%')))
		*p = 0;
 
	DEBUG(3,("api_DosPrintQGetInfo uLevel=%d name=%s\n",uLevel,QueueName));
 
	/* check it's a supported varient */
	if (!prefix_ok(str1,"zWrLh"))
		return False;
	if (!check_printq_info(&desc,uLevel,str2,str3)) {
		/*
		 * Patch from Scott Moomaw <scott@bridgewater.edu>
		 * to return the 'invalid info level' error if an
		 * unknown level was requested.
		 */
		*rdata_len = 0;
		*rparam_len = 6;
		*rparam = smb_realloc_limit(*rparam,*rparam_len);
		if (!*rparam) {
			return False;
		}
		SSVALS(*rparam,0,ERRunknownlevel);
		SSVAL(*rparam,2,0);
		SSVAL(*rparam,4,0);
		return(True);
	}
 
	snum = find_service(QueueName);
	if ( !(lp_snum_ok(snum) && lp_print_ok(snum)) )
		return False;
		
	if (uLevel==52) {
		count = get_printerdrivernumber(snum);
		DEBUG(3,("api_DosPrintQGetInfo: Driver files count: %d\n",count));
	} else {
		count = print_queue_status(snum, &queue,&status);
	}

	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			SAFE_FREE(queue);
			return False;
		}
		desc.base = *rdata;
		desc.buflen = mdrcnt;
	} else {
		/*
		 * Don't return data but need to get correct length
		 * init_package will return wrong size if buflen=0
		 */
		desc.buflen = getlen(desc.format);
		desc.base = tmpdata = (char *) SMB_MALLOC (desc.buflen);
	}

	if (init_package(&desc,1,count)) {
		desc.subcount = count;
		fill_printq_info(conn,snum,uLevel,&desc,count,queue,&status);
	}

	*rdata_len = desc.usedlen;
  
	/*
	 * We must set the return code to ERRbuftoosmall
	 * in order to support lanman style printing with Win NT/2k
	 * clients       --jerry
	 */
	if (!mdrcnt && lp_disable_spoolss())
		desc.errcode = ERRbuftoosmall;
 
	*rdata_len = desc.usedlen;
	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		SAFE_FREE(queue);
		SAFE_FREE(tmpdata);
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,desc.neededlen);
  
	DEBUG(4,("printqgetinfo: errorcode %d\n",desc.errcode));

	SAFE_FREE(queue);
	SAFE_FREE(tmpdata);

	return(True);
#endif
}

/****************************************************************************
 View list of all print jobs on all queues.
****************************************************************************/

static bool api_DosPrintQEnum(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt, int mprcnt,
				char **rdata, char** rparam,
				int *rdata_len, int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *param_format = get_safe_str_ptr(param,tpscnt,param,2);
	char *output_format1 = skip_string(param,tpscnt,param_format);
	char *p = skip_string(param,tpscnt,output_format1);
	unsigned int uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	char *output_format2 = get_safe_str_ptr(param,tpscnt,p,4);
	int services = lp_numservices();
	int i, n;
	struct pack_desc desc;
	print_queue_struct **queue = NULL;
	print_status_struct *status = NULL;
	int *subcntarr = NULL;
	int queuecnt = 0, subcnt = 0, succnt = 0;
 
	if (!param_format || !output_format1 || !p) {
		return False;
	}

	memset((char *)&desc,'\0',sizeof(desc));

	DEBUG(3,("DosPrintQEnum uLevel=%d\n",uLevel));
 
	if (!prefix_ok(param_format,"WrLeh")) {
		return False;
	}
	if (!check_printq_info(&desc,uLevel,output_format1,output_format2)) {
		/*
		 * Patch from Scott Moomaw <scott@bridgewater.edu>
		 * to return the 'invalid info level' error if an
		 * unknown level was requested.
		 */
		*rdata_len = 0;
		*rparam_len = 6;
		*rparam = smb_realloc_limit(*rparam,*rparam_len);
		if (!*rparam) {
			return False;
		}
		SSVALS(*rparam,0,ERRunknownlevel);
		SSVAL(*rparam,2,0);
		SSVAL(*rparam,4,0);
		return(True);
	}

	for (i = 0; i < services; i++) {
		if (lp_snum_ok(i) && lp_print_ok(i) && lp_browseable(i)) {
			queuecnt++;
		}
	}

	if((queue = SMB_MALLOC_ARRAY(print_queue_struct*, queuecnt)) == NULL) {
		DEBUG(0,("api_DosPrintQEnum: malloc fail !\n"));
		goto err;
	}
	memset(queue,0,queuecnt*sizeof(print_queue_struct*));
	if((status = SMB_MALLOC_ARRAY(print_status_struct,queuecnt)) == NULL) {
		DEBUG(0,("api_DosPrintQEnum: malloc fail !\n"));
		goto err;
	}
	memset(status,0,queuecnt*sizeof(print_status_struct));
	if((subcntarr = SMB_MALLOC_ARRAY(int,queuecnt)) == NULL) {
		DEBUG(0,("api_DosPrintQEnum: malloc fail !\n"));
		goto err;
	}

	subcnt = 0;
	n = 0;
	for (i = 0; i < services; i++) {
		if (lp_snum_ok(i) && lp_print_ok(i) && lp_browseable(i)) {
			subcntarr[n] = print_queue_status(i, &queue[n],&status[n]);
			subcnt += subcntarr[n];
			n++;
		}
	}

	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			goto err;
		}
	}
	desc.base = *rdata;
	desc.buflen = mdrcnt;

	if (init_package(&desc,queuecnt,subcnt)) {
		n = 0;
		succnt = 0;
		for (i = 0; i < services; i++) {
			if (lp_snum_ok(i) && lp_print_ok(i) && lp_browseable(i)) {
				fill_printq_info(conn,i,uLevel,&desc,subcntarr[n],queue[n],&status[n]);
				n++;
				if (desc.errcode == NERR_Success) {
					succnt = n;
				}
			}
		}
	}

	SAFE_FREE(subcntarr);
 
	*rdata_len = desc.usedlen;
	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		goto err;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,succnt);
	SSVAL(*rparam,6,queuecnt);
  
	for (i = 0; i < queuecnt; i++) {
		if (queue) {
			SAFE_FREE(queue[i]);
		}
	}

	SAFE_FREE(queue);
	SAFE_FREE(status);
  
	return True;

  err:

	SAFE_FREE(subcntarr);
	for (i = 0; i < queuecnt; i++) {
		if (queue) {
			SAFE_FREE(queue[i]);
		}
	}
	SAFE_FREE(queue);
	SAFE_FREE(status);

	return False;
#endif
}

/****************************************************************************
 Get info level for a server list query.
****************************************************************************/

static bool check_server_info(int uLevel, char* id)
{
	switch( uLevel ) {
		case 0:
			if (strcmp(id,"B16") != 0) {
				return False;
			}
			break;
		case 1:
			if (strcmp(id,"B16BBDz") != 0) {
				return False;
			}
			break;
		default: 
			return False;
	}
	return True;
}

struct srv_info_struct {
	fstring name;
	uint32 type;
	fstring comment;
	fstring domain;
	bool server_added;
};

/*******************************************************************
 Get server info lists from the files saved by nmbd. Return the
 number of entries.
******************************************************************/

static int get_server_info(uint32 servertype, 
			   struct srv_info_struct **servers,
			   const char *domain)
{
	int count=0;
	int alloced=0;
	char **lines;
	bool local_list_only;
	int i;

	lines = file_lines_load(lock_path(SERVER_LIST), NULL, 0);
	if (!lines) {
		DEBUG(4,("Can't open %s - %s\n",lock_path(SERVER_LIST),strerror(errno)));
		return 0;
	}

	/* request for everything is code for request all servers */
	if (servertype == SV_TYPE_ALL) {
		servertype &= ~(SV_TYPE_DOMAIN_ENUM|SV_TYPE_LOCAL_LIST_ONLY);
	}

	local_list_only = (servertype & SV_TYPE_LOCAL_LIST_ONLY);

	DEBUG(4,("Servertype search: %8x\n",servertype));

	for (i=0;lines[i];i++) {
		fstring stype;
		struct srv_info_struct *s;
		const char *ptr = lines[i];
		bool ok = True;
		TALLOC_CTX *frame = NULL;
		char *p;

		if (!*ptr) {
			continue;
		}

		if (count == alloced) {
			alloced += 10;
			*servers = SMB_REALLOC_ARRAY(*servers,struct srv_info_struct, alloced);
			if (!*servers) {
				DEBUG(0,("get_server_info: failed to enlarge servers info struct!\n"));
				file_lines_free(lines);
				return 0;
			}
			memset((char *)((*servers)+count),'\0',sizeof(**servers)*(alloced-count));
		}
		s = &(*servers)[count];

		frame = talloc_stackframe();
		s->name[0] = '\0';
		if (!next_token_talloc(frame,&ptr,&p, NULL)) {
			TALLOC_FREE(frame);
			continue;
		}
		fstrcpy(s->name, p);

		stype[0] = '\0';
		if (!next_token_talloc(frame,&ptr, &p, NULL)) {
			TALLOC_FREE(frame);
			continue;
		}
		fstrcpy(stype, p);

		s->comment[0] = '\0';
		if (!next_token_talloc(frame,&ptr, &p, NULL)) {
			TALLOC_FREE(frame);
			continue;
		}
		fstrcpy(s->comment, p);
		string_truncate(s->comment, MAX_SERVER_STRING_LENGTH);

		s->domain[0] = '\0';
		if (!next_token_talloc(frame,&ptr,&p, NULL)) {
			/* this allows us to cope with an old nmbd */
			fstrcpy(s->domain,lp_workgroup());
		} else {
			fstrcpy(s->domain, p);
		}
		TALLOC_FREE(frame);

		if (sscanf(stype,"%X",&s->type) != 1) {
			DEBUG(4,("r:host file "));
			ok = False;
		}

		/* Filter the servers/domains we return based on what was asked for. */

		/* Check to see if we are being asked for a local list only. */
		if(local_list_only && ((s->type & SV_TYPE_LOCAL_LIST_ONLY) == 0)) {
			DEBUG(4,("r: local list only"));
			ok = False;
		}

		/* doesn't match up: don't want it */
		if (!(servertype & s->type)) {
			DEBUG(4,("r:serv type "));
			ok = False;
		}

		if ((servertype & SV_TYPE_DOMAIN_ENUM) != 
				(s->type & SV_TYPE_DOMAIN_ENUM)) {
			DEBUG(4,("s: dom mismatch "));
			ok = False;
		}
    
		if (!strequal(domain, s->domain) && !(servertype & SV_TYPE_DOMAIN_ENUM)) {
			ok = False;
		}
    
		/* We should never return a server type with a SV_TYPE_LOCAL_LIST_ONLY set. */
		s->type &= ~SV_TYPE_LOCAL_LIST_ONLY;

		if (ok) {
			DEBUG(4,("**SV** %20s %8x %25s %15s\n",
				s->name, s->type, s->comment, s->domain));
			s->server_added = True;
			count++;
		} else {
			DEBUG(4,("%20s %8x %25s %15s\n",
				s->name, s->type, s->comment, s->domain));
		}
	}
  
	file_lines_free(lines);
	return count;
}

/*******************************************************************
 Fill in a server info structure.
******************************************************************/

static int fill_srv_info(struct srv_info_struct *service, 
			 int uLevel, char **buf, int *buflen, 
			 char **stringbuf, int *stringspace, char *baseaddr)
{
	int struct_len;
	char* p;
	char* p2;
	int l2;
	int len;
 
	switch (uLevel) {
		case 0:
			struct_len = 16;
			break;
		case 1:
			struct_len = 26;
			break;
		default:
			return -1;
	}
 
	if (!buf) {
		len = 0;
		switch (uLevel) {
			case 1:
				len = strlen(service->comment)+1;
				break;
		}

		*buflen = struct_len;
		*stringspace = len;
		return struct_len + len;
	}
  
	len = struct_len;
	p = *buf;
	if (*buflen < struct_len) {
		return -1;
	}
	if (stringbuf) {
		p2 = *stringbuf;
		l2 = *stringspace;
	} else {
		p2 = p + struct_len;
		l2 = *buflen - struct_len;
	}
	if (!baseaddr) {
		baseaddr = p;
	}
  
	switch (uLevel) {
		case 0:
			push_ascii(p,service->name, MAX_NETBIOSNAME_LEN, STR_TERMINATE);
			break;

		case 1:
			push_ascii(p,service->name,MAX_NETBIOSNAME_LEN, STR_TERMINATE);
			SIVAL(p,18,service->type);
			SIVAL(p,22,PTR_DIFF(p2,baseaddr));
			len += CopyAndAdvance(&p2,service->comment,&l2);
			break;
	}

	if (stringbuf) {
		*buf = p + struct_len;
		*buflen -= struct_len;
		*stringbuf = p2;
		*stringspace = l2;
	} else {
		*buf = p2;
		*buflen -= len;
	}
	return len;
}


static bool srv_comp(struct srv_info_struct *s1,struct srv_info_struct *s2)
{
	return(strcmp(s1->name,s2->name));
}

/****************************************************************************
 View list of servers available (or possibly domains). The info is
 extracted from lists saved by nmbd on the local host.
****************************************************************************/

static bool api_RNetServerEnum(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt, int mprcnt, char **rdata, 
				char **rparam, int *rdata_len, int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param, tpscnt, param, 2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel = get_safe_SVAL(param, tpscnt, p, 0, -1);
	int buf_len = get_safe_SVAL(param,tpscnt, p, 2, 0);
	uint32 servertype = get_safe_IVAL(param,tpscnt,p,4, 0);
	char *p2;
	int data_len, fixed_len, string_len;
	int f_len = 0, s_len = 0;
	struct srv_info_struct *servers=NULL;
	int counted=0,total=0;
	int i,missed;
	fstring domain;
	bool domain_request;
	bool local_request;

	if (!str1 || !str2 || !p) {
		return False;
	}

	/* If someone sets all the bits they don't really mean to set
	   DOMAIN_ENUM and LOCAL_LIST_ONLY, they just want all the
	   known servers. */

	if (servertype == SV_TYPE_ALL) {
		servertype &= ~(SV_TYPE_DOMAIN_ENUM|SV_TYPE_LOCAL_LIST_ONLY);
	}

	/* If someone sets SV_TYPE_LOCAL_LIST_ONLY but hasn't set
	   any other bit (they may just set this bit on its own) they 
	   want all the locally seen servers. However this bit can be 
	   set on its own so set the requested servers to be 
	   ALL - DOMAIN_ENUM. */

	if ((servertype & SV_TYPE_LOCAL_LIST_ONLY) && !(servertype & SV_TYPE_DOMAIN_ENUM)) {
		servertype = SV_TYPE_ALL & ~(SV_TYPE_DOMAIN_ENUM);
	}

	domain_request = ((servertype & SV_TYPE_DOMAIN_ENUM) != 0);
	local_request = ((servertype & SV_TYPE_LOCAL_LIST_ONLY) != 0);

	p += 8;

	if (!prefix_ok(str1,"WrLehD")) {
		return False;
	}
	if (!check_server_info(uLevel,str2)) {
		return False;
	}
  
	DEBUG(4, ("server request level: %s %8x ", str2, servertype));
	DEBUG(4, ("domains_req:%s ", BOOLSTR(domain_request)));
	DEBUG(4, ("local_only:%s\n", BOOLSTR(local_request)));

	if (strcmp(str1, "WrLehDz") == 0) {
		if (skip_string(param,tpscnt,p) == NULL) {
			return False;
		}
		pull_ascii_fstring(domain, p);
	} else {
		fstrcpy(domain, lp_workgroup());
	}

	if (lp_browse_list()) {
		total = get_server_info(servertype,&servers,domain);
	}

	data_len = fixed_len = string_len = 0;
	missed = 0;

	if (total > 0) {
		qsort(servers,total,sizeof(servers[0]),QSORT_CAST srv_comp);
	}

	{
		char *lastname=NULL;

		for (i=0;i<total;i++) {
			struct srv_info_struct *s = &servers[i];

			if (lastname && strequal(lastname,s->name)) {
				continue;
			}
			lastname = s->name;
			data_len += fill_srv_info(s,uLevel,0,&f_len,0,&s_len,0);
			DEBUG(4,("fill_srv_info %20s %8x %25s %15s\n",
				s->name, s->type, s->comment, s->domain));
      
			if (data_len <= buf_len) {
				counted++;
				fixed_len += f_len;
				string_len += s_len;
			} else {
				missed++;
			}
		}
	}

	*rdata_len = fixed_len + string_len;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}
  
	p2 = (*rdata) + fixed_len;	/* auxilliary data (strings) will go here */
	p = *rdata;
	f_len = fixed_len;
	s_len = string_len;

	{
		char *lastname=NULL;
		int count2 = counted;

		for (i = 0; i < total && count2;i++) {
			struct srv_info_struct *s = &servers[i];

			if (lastname && strequal(lastname,s->name)) {
				continue;
			}
			lastname = s->name;
			fill_srv_info(s,uLevel,&p,&f_len,&p2,&s_len,*rdata);
			DEBUG(4,("fill_srv_info %20s %8x %25s %15s\n",
				s->name, s->type, s->comment, s->domain));
			count2--;
		}
	}
  
	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVAL(*rparam,0,(missed == 0 ? NERR_Success : ERRmoredata));
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,counted);
	SSVAL(*rparam,6,counted+missed);

	SAFE_FREE(servers);

	DEBUG(3,("NetServerEnum domain = %s uLevel=%d counted=%d total=%d\n",
		domain,uLevel,counted,counted+missed));

	return True;
}

/****************************************************************************
  command 0x34 - suspected of being a "Lookup Names" stub api
  ****************************************************************************/

static bool api_RNetGroupGetUsers(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt, int mprcnt, char **rdata, 
				char **rparam, int *rdata_len, int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	int buf_len = get_safe_SVAL(param,tpscnt,p,2,0);
	int counted=0;
	int missed=0;

	if (!str1 || !str2 || !p) {
		return False;
	}

	DEBUG(5,("RNetGroupGetUsers: %s %s %s %d %d\n",
		str1, str2, p, uLevel, buf_len));

	if (!prefix_ok(str1,"zWrLeh")) {
		return False;
	}
  
	*rdata_len = 0;
  
	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	SSVAL(*rparam,0,0x08AC); /* informational warning message */
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,counted);
	SSVAL(*rparam,6,counted+missed);

	return True;
}

/****************************************************************************
  get info about a share
  ****************************************************************************/

static bool check_share_info(int uLevel, char* id)
{
	switch( uLevel ) {
		case 0:
			if (strcmp(id,"B13") != 0) {
				return False;
			}
			break;
		case 1:
			if (strcmp(id,"B13BWz") != 0) {
				return False;
			}
			break;
		case 2:
			if (strcmp(id,"B13BWzWWWzB9B") != 0) {
				return False;
			}
			break;
		case 91:
			if (strcmp(id,"B13BWzWWWzB9BB9BWzWWzWW") != 0) {
				return False;
			}
			break;
		default:
			return False;
	}
	return True;
}

static int fill_share_info(connection_struct *conn, int snum, int uLevel,
 			   char** buf, int* buflen,
 			   char** stringbuf, int* stringspace, char* baseaddr)
{
	int struct_len;
	char* p;
	char* p2;
	int l2;
	int len;
 
	switch( uLevel ) {
		case 0:
			struct_len = 13;
			break;
		case 1:
			struct_len = 20;
			break;
		case 2:
			struct_len = 40;
			break;
		case 91:
			struct_len = 68;
			break;
		default:
			return -1;
	}
  
 
	if (!buf) {
		len = 0;

		if (uLevel > 0) {
			len += StrlenExpanded(conn,snum,lp_comment(snum));
		}
		if (uLevel > 1) {
			len += strlen(lp_pathname(snum)) + 1;
		}
		if (buflen) {
			*buflen = struct_len;
		}
		if (stringspace) {
			*stringspace = len;
		}
		return struct_len + len;
	}
  
	len = struct_len;
	p = *buf;
	if ((*buflen) < struct_len) {
		return -1;
	}

	if (stringbuf) {
		p2 = *stringbuf;
		l2 = *stringspace;
	} else {
		p2 = p + struct_len;
		l2 = (*buflen) - struct_len;
	}

	if (!baseaddr) {
		baseaddr = p;
	}
  
	push_ascii(p,lp_servicename(snum),13, STR_TERMINATE);
  
	if (uLevel > 0) {
		int type;

		SCVAL(p,13,0);
		type = STYPE_DISKTREE;
#ifndef AVM_NO_PRINTING
		if (lp_print_ok(snum)) {
			type = STYPE_PRINTQ;
		}
#endif
		if (strequal("IPC",lp_fstype(snum))) {
			type = STYPE_IPC;
		}
		SSVAL(p,14,type);		/* device type */
		SIVAL(p,16,PTR_DIFF(p2,baseaddr));
		len += CopyExpanded(conn,snum,&p2,lp_comment(snum),&l2);
	}
  
	if (uLevel > 1) {
		SSVAL(p,20,ACCESS_READ|ACCESS_WRITE|ACCESS_CREATE); /* permissions */
		SSVALS(p,22,-1);		/* max uses */
		SSVAL(p,24,1); /* current uses */
		SIVAL(p,26,PTR_DIFF(p2,baseaddr)); /* local pathname */
		len += CopyAndAdvance(&p2,lp_pathname(snum),&l2);
		memset(p+30,0,SHPWLEN+2); /* passwd (reserved), pad field */
	}
  
	if (uLevel > 2) {
		memset(p+40,0,SHPWLEN+2);
		SSVAL(p,50,0);
		SIVAL(p,52,0);
		SSVAL(p,56,0);
		SSVAL(p,58,0);
		SIVAL(p,60,0);
		SSVAL(p,64,0);
		SSVAL(p,66,0);
	}
       
	if (stringbuf) {
		(*buf) = p + struct_len;
		(*buflen) -= struct_len;
		(*stringbuf) = p2;
		(*stringspace) = l2;
	} else {
		(*buf) = p2;
		(*buflen) -= len;
	}

	return len;
}

static bool api_RNetShareGetInfo(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *netname = skip_string(param,tpscnt,str2);
	char *p = skip_string(param,tpscnt,netname);
	int uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	int snum;
  
	if (!str1 || !str2 || !netname || !p) {
		return False;
	}

	snum = find_service(netname);
	if (snum < 0) {
		return False;
	}
  
	/* check it's a supported varient */
	if (!prefix_ok(str1,"zWrLh")) {
		return False;
	}
	if (!check_share_info(uLevel,str2)) {
		return False;
	}
 
	*rdata = smb_realloc_limit(*rdata,mdrcnt);
	if (!*rdata) {
		return False;
	}
	p = *rdata;
	*rdata_len = fill_share_info(conn,snum,uLevel,&p,&mdrcnt,0,0,0);
	if (*rdata_len < 0) {
		return False;
	}
 
	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVAL(*rparam,0,NERR_Success);
	SSVAL(*rparam,2,0);		/* converter word */
	SSVAL(*rparam,4,*rdata_len);
 
	return True;
}

/****************************************************************************
  View the list of available shares.

  This function is the server side of the NetShareEnum() RAP call.
  It fills the return buffer with share names and share comments.
  Note that the return buffer normally (in all known cases) allows only
  twelve byte strings for share names (plus one for a nul terminator).
  Share names longer than 12 bytes must be skipped.
 ****************************************************************************/

static bool api_RNetShareEnum( connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int                mdrcnt,
				int                mprcnt,
				char             **rdata,
				char             **rparam,
				int               *rdata_len,
				int               *rparam_len )
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	int buf_len = get_safe_SVAL(param,tpscnt,p,2,0);
	char *p2;
	int count = 0;
	int total=0,counted=0;
	bool missed = False;
	int i;
	int data_len, fixed_len, string_len;
	int f_len = 0, s_len = 0;
 
	if (!str1 || !str2 || !p) {
		return False;
	}

	if (!prefix_ok(str1,"WrLeh")) {
		return False;
	}
	if (!check_share_info(uLevel,str2)) {
		return False;
	}
  
	/* Ensure all the usershares are loaded. */
	become_root();
#if 0 /* tonywu */
	load_registry_shares();
	count = load_usershare_shares();
#else
	count = lp_numservices();
#endif
	unbecome_root();

	data_len = fixed_len = string_len = 0;
	for (i=0;i<count;i++) {
		fstring servicename_dos;
		if (!(lp_browseable(i) && lp_snum_ok(i))) {
			continue;
		}
		push_ascii_fstring(servicename_dos, lp_servicename(i));
		/* Maximum name length = 13. */
		if( lp_browseable( i ) && lp_snum_ok( i ) && (strlen(servicename_dos) < 13)) {
			total++;
			data_len += fill_share_info(conn,i,uLevel,0,&f_len,0,&s_len,0);
			if (data_len <= buf_len) {
				counted++;
				fixed_len += f_len;
				string_len += s_len;
			} else {
				missed = True;
			}
		}
	}

	*rdata_len = fixed_len + string_len;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}
 
	p2 = (*rdata) + fixed_len;	/* auxiliary data (strings) will go here */
	p = *rdata;
	f_len = fixed_len;
	s_len = string_len;

	for( i = 0; i < count; i++ ) {
		fstring servicename_dos;
		if (!(lp_browseable(i) && lp_snum_ok(i))) {
			continue;
		}

		push_ascii_fstring(servicename_dos, lp_servicename(i));
		if (lp_browseable(i) && lp_snum_ok(i) && (strlen(servicename_dos) < 13)) {
			if (fill_share_info( conn,i,uLevel,&p,&f_len,&p2,&s_len,*rdata ) < 0) {
				break;
			}
		}
	}
  
	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVAL(*rparam,0,missed ? ERRmoredata : NERR_Success);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,counted);
	SSVAL(*rparam,6,total);
  
	DEBUG(3,("RNetShareEnum gave %d entries of %d (%d %d %d %d)\n",
		counted,total,uLevel,
		buf_len,*rdata_len,mdrcnt));

	return True;
}

/****************************************************************************
  Add a share
  ****************************************************************************/

static bool api_RNetShareAdd(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	fstring sharename;
	fstring comment;
	char *pathname = NULL;
	char *command, *cmdname;
	unsigned int offset;
	int snum;
	int res = ERRunsup;

	if (!str1 || !str2 || !p) {
		return False;
	}

	/* check it's a supported varient */
	if (!prefix_ok(str1,RAP_WShareAdd_REQ)) {
		return False;
	}
	if (!check_share_info(uLevel,str2)) {
		return False;
	}
	if (uLevel != 2) {
		return False;
	}

	/* Do we have a string ? */
	if (skip_string(data,mdrcnt,data) == NULL) {
		return False;
	}
	pull_ascii_fstring(sharename,data);
	snum = find_service(sharename);
	if (snum >= 0) { /* already exists */
		res = ERRfilexists;
		goto error_exit;
	}

	if (mdrcnt < 28) {
		return False;
	}

	/* only support disk share adds */
	if (SVAL(data,14)!=STYPE_DISKTREE) {
		return False;
	}

	offset = IVAL(data, 16);
	if (offset >= mdrcnt) {
		res = ERRinvalidparam;
		goto error_exit;
	}

	/* Do we have a string ? */
	if (skip_string(data,mdrcnt,data+offset) == NULL) {
		return False;
	}
	pull_ascii_fstring(comment, offset? (data+offset) : "");

	offset = IVAL(data, 26);

	if (offset >= mdrcnt) {
		res = ERRinvalidparam;
		goto error_exit;
	}

	/* Do we have a string ? */
	if (skip_string(data,mdrcnt,data+offset) == NULL) {
		return False;
	}

	pull_ascii_talloc(talloc_tos(), &pathname, offset? (data+offset) : "");
	if (!pathname) {
		return false;
	}

	string_replace(sharename, '"', ' ');
	string_replace(pathname, '"', ' ');
	string_replace(comment, '"', ' ');

	cmdname = lp_add_share_cmd();

	if (!cmdname || *cmdname == '\0') {
		return False;
	}

	if (asprintf(&command, "%s \"%s\" \"%s\" \"%s\" \"%s\"",
		     lp_add_share_cmd(), dyn_CONFIGFILE, sharename,
		     pathname, comment) == -1) {
		return false;
	}

	DEBUG(10,("api_RNetShareAdd: Running [%s]\n", command ));

	if ((res = smbrun(command, NULL)) != 0) {
		DEBUG(1,("api_RNetShareAdd: Running [%s] returned (%d)\n",
			 command, res ));
		SAFE_FREE(command);
		res = ERRnoaccess;
		goto error_exit;
	} else {
		SAFE_FREE(command);
		message_send_all(smbd_messaging_context(),
				 MSG_SMB_CONF_UPDATED, NULL, 0, NULL);
	}

	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVAL(*rparam,0,NERR_Success);
	SSVAL(*rparam,2,0);		/* converter word */
	SSVAL(*rparam,4,*rdata_len);
	*rdata_len = 0;
  
	return True;

  error_exit:

	*rparam_len = 4;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	*rdata_len = 0;
	SSVAL(*rparam,0,res);
	SSVAL(*rparam,2,0);
	return True;
}

/****************************************************************************
  view list of groups available
  ****************************************************************************/

static bool api_RNetGroupEnum(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	int i;
	int errflags=0;
	int resume_context, cli_buf_size;
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);

	struct pdb_search *search;
	struct samr_displayentry *entries;

	int num_entries;
 
	if (!str1 || !str2 || !p) {
		return False;
	}

	if (strcmp(str1,"WrLeh") != 0) {
		return False;
	}

	/* parameters  
	 * W-> resume context (number of users to skip)
	 * r -> return parameter pointer to receive buffer 
	 * L -> length of receive buffer
	 * e -> return parameter number of entries
	 * h -> return parameter total number of users
	 */

	if (strcmp("B21",str2) != 0) {
		return False;
	}

	/* get list of domain groups SID_DOMAIN_GRP=2 */
	become_root();
	search = pdb_search_groups();
	unbecome_root();

	if (search == NULL) {
		DEBUG(3,("api_RNetGroupEnum:failed to get group list"));
		return False;
	}

	resume_context = get_safe_SVAL(param,tpscnt,p,0,-1);
	cli_buf_size= get_safe_SVAL(param,tpscnt,p,2,0);
	DEBUG(10,("api_RNetGroupEnum:resume context: %d, client buffer size: "
		  "%d\n", resume_context, cli_buf_size));

	become_root();
	num_entries = pdb_search_entries(search, resume_context, 0xffffffff,
					 &entries);
	unbecome_root();

	*rdata_len = cli_buf_size;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}

	p = *rdata;

	for(i=0; i<num_entries; i++) {
		fstring name;
		fstrcpy(name, entries[i].account_name);
		if( ((PTR_DIFF(p,*rdata)+21) <= *rdata_len) ) {
			/* truncate the name at 21 chars. */
			memcpy(p, name, 21); 
			DEBUG(10,("adding entry %d group %s\n", i, p));
			p += 21;
			p += 5; /* Both NT4 and W2k3SP1 do padding here.
				   No idea why... */
		} else {
			/* set overflow error */
			DEBUG(3,("overflow on entry %d group %s\n", i, name));
			errflags=234;
			break;
		}
	}

	pdb_search_destroy(search);

	*rdata_len = PTR_DIFF(p,*rdata);

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
  	SSVAL(*rparam, 0, errflags);
  	SSVAL(*rparam, 2, 0);		/* converter word */
  	SSVAL(*rparam, 4, i);	/* is this right?? */
 	SSVAL(*rparam, 6, resume_context+num_entries);	/* is this right?? */

	return(True);
}

/*******************************************************************
 Get groups that a user is a member of.
******************************************************************/

static bool api_NetUserGetGroups(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *UserName = skip_string(param,tpscnt,str2);
	char *p = skip_string(param,tpscnt,UserName);
	int uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	const char *level_string;
	int count=0;
	struct samu *sampw = NULL;
	bool ret = False;
	DOM_SID *sids;
	gid_t *gids;
	size_t num_groups;
	size_t i;
	NTSTATUS result;
	DOM_SID user_sid;
	enum lsa_SidType type;
	char *endp = NULL;
	TALLOC_CTX *mem_ctx;

	if (!str1 || !str2 || !UserName || !p) {
		return False;
	}

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	/* check it's a supported varient */

	if ( strcmp(str1,"zWrLeh") != 0 )
		return False;

	switch( uLevel ) {
		case 0:
			level_string = "B21";
			break;
		default:
			return False;
	}

	if (strcmp(level_string,str2) != 0)
		return False;

	*rdata_len = mdrcnt + 1024;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}

	SSVAL(*rparam,0,NERR_Success);
	SSVAL(*rparam,2,0);		/* converter word */

	p = *rdata;
	endp = *rdata + *rdata_len;

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		DEBUG(0, ("talloc_new failed\n"));
		return False;
	}

	if ( !(sampw = samu_new(mem_ctx)) ) {
		DEBUG(0, ("samu_new() failed!\n"));
		TALLOC_FREE(mem_ctx);
		return False;
	}

	/* Lookup the user information; This should only be one of
	   our accounts (not remote domains) */

	become_root();					/* ROOT BLOCK */

	if (!lookup_name(mem_ctx, UserName, LOOKUP_NAME_ALL,
			 NULL, NULL, &user_sid, &type)) {
		DEBUG(10, ("lookup_name(%s) failed\n", UserName));
		goto done;
	}

	if (type != SID_NAME_USER) {
		DEBUG(10, ("%s is a %s, not a user\n", UserName,
			   sid_type_lookup(type)));
		goto done;
	}

	if ( !pdb_getsampwsid(sampw, &user_sid) ) {
		DEBUG(10, ("pdb_getsampwsid(%s) failed for user %s\n",
			   sid_string_dbg(&user_sid), UserName));
		goto done;
	}

	gids = NULL;
	sids = NULL;
	num_groups = 0;

	result = pdb_enum_group_memberships(mem_ctx, sampw,
					    &sids, &gids, &num_groups);

	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(10, ("pdb_enum_group_memberships failed for %s\n",
			   UserName));
		goto done;
	}

	for (i=0; i<num_groups; i++) {
		const char *grp_name;

		if ( lookup_sid(mem_ctx, &sids[i], NULL, &grp_name, NULL) ) {
			strlcpy(p, grp_name, PTR_DIFF(endp,p));
			p += 21;
			count++;
		}
	}

	*rdata_len = PTR_DIFF(p,*rdata);

	SSVAL(*rparam,4,count);	/* is this right?? */
	SSVAL(*rparam,6,count);	/* is this right?? */

	ret = True;

done:
	unbecome_root();				/* END ROOT BLOCK */

	TALLOC_FREE(mem_ctx);

	return ret;
}

/*******************************************************************
 Get all users.
******************************************************************/

static bool api_RNetUserEnum(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	int count_sent=0;
	int num_users=0;
	int errflags=0;
	int i, resume_context, cli_buf_size;
	struct pdb_search *search;
	struct samr_displayentry *users;

	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	char *endp = NULL;

	if (!str1 || !str2 || !p) {
		return False;
	}

	if (strcmp(str1,"WrLeh") != 0)
		return False;
	/* parameters
	  * W-> resume context (number of users to skip)
	  * r -> return parameter pointer to receive buffer
	  * L -> length of receive buffer
	  * e -> return parameter number of entries
	  * h -> return parameter total number of users
	  */

	resume_context = get_safe_SVAL(param,tpscnt,p,0,-1);
	cli_buf_size= get_safe_SVAL(param,tpscnt,p,2,0);
	DEBUG(10,("api_RNetUserEnum:resume context: %d, client buffer size: %d\n",
			resume_context, cli_buf_size));

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	/* check it's a supported varient */
	if (strcmp("B21",str2) != 0)
		return False;

	*rdata_len = cli_buf_size;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}

	p = *rdata;
	endp = *rdata + *rdata_len;

	become_root();
	search = pdb_search_users(ACB_NORMAL);
	unbecome_root();
	if (search == NULL) {
		DEBUG(0, ("api_RNetUserEnum:unable to open sam database.\n"));
		return False;
	}

	become_root();
	num_users = pdb_search_entries(search, resume_context, 0xffffffff,
				       &users);
	unbecome_root();

	errflags=NERR_Success;

	for (i=0; i<num_users; i++) {
		const char *name = users[i].account_name;

		if(((PTR_DIFF(p,*rdata)+21)<=*rdata_len)&&(strlen(name)<=21)) {
			strlcpy(p,name,PTR_DIFF(endp,p));
			DEBUG(10,("api_RNetUserEnum:adding entry %d username "
				  "%s\n",count_sent,p));
			p += 21;
			count_sent++;
		} else {
			/* set overflow error */
			DEBUG(10,("api_RNetUserEnum:overflow on entry %d "
				  "username %s\n",count_sent,name));
			errflags=234;
			break;
		}
	}

	pdb_search_destroy(search);

	*rdata_len = PTR_DIFF(p,*rdata);

	SSVAL(*rparam,0,errflags);
	SSVAL(*rparam,2,0);	      /* converter word */
	SSVAL(*rparam,4,count_sent);  /* is this right?? */
	SSVAL(*rparam,6,num_users); /* is this right?? */

	return True;
}

/****************************************************************************
 Get the time of day info.
****************************************************************************/

static bool api_NetRemoteTOD(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	struct tm *t;
	time_t unixdate = time(NULL);
	char *p;

	*rparam_len = 4;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	*rdata_len = 21;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}

	SSVAL(*rparam,0,NERR_Success);
	SSVAL(*rparam,2,0);		/* converter word */

	p = *rdata;

	srv_put_dos_date3(p,0,unixdate); /* this is the time that is looked at
					    by NT in a "net time" operation,
					    it seems to ignore the one below */

	/* the client expects to get localtime, not GMT, in this bit 
		(I think, this needs testing) */
	t = localtime(&unixdate);
	if (!t) {
		return False;
	}

	SIVAL(p,4,0);		/* msecs ? */
	SCVAL(p,8,t->tm_hour);
	SCVAL(p,9,t->tm_min);
	SCVAL(p,10,t->tm_sec);
	SCVAL(p,11,0);		/* hundredths of seconds */
	SSVALS(p,12,get_time_zone(unixdate)/60); /* timezone in minutes from GMT */
	SSVAL(p,14,10000);		/* timer interval in 0.0001 of sec */
	SCVAL(p,16,t->tm_mday);
	SCVAL(p,17,t->tm_mon + 1);
	SSVAL(p,18,1900+t->tm_year);
	SCVAL(p,20,t->tm_wday);

	return True;
}

/****************************************************************************
 Set the user password.
*****************************************************************************/

static bool api_SetUserPassword(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#if ALLOW_CHANGE_PASSWORD
	char *np = get_safe_str_ptr(param,tpscnt,param,2);
	char *p = NULL;
	fstring user;
	fstring pass1,pass2;

	/* Skip 2 strings. */
	p = skip_string(param,tpscnt,np);
	p = skip_string(param,tpscnt,p);

	if (!np || !p) {
		return False;
	}

	/* Do we have a string ? */
	if (skip_string(param,tpscnt,p) == NULL) {
		return False;
	}
	pull_ascii_fstring(user,p);

	p = skip_string(param,tpscnt,p);
	if (!p) {
		return False;
	}

	memset(pass1,'\0',sizeof(pass1));
	memset(pass2,'\0',sizeof(pass2));
	/*
	 * We use 31 here not 32 as we're checking
	 * the last byte we want to access is safe.
	 */
	if (!is_offset_safe(param,tpscnt,p,31)) {
		return False;
	}
	memcpy(pass1,p,16);
	memcpy(pass2,p+16,16);

	*rparam_len = 4;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	*rdata_len = 0;

	SSVAL(*rparam,0,NERR_badpass);
	SSVAL(*rparam,2,0);		/* converter word */

	DEBUG(3,("Set password for <%s>\n",user));

	/*
	 * Attempt to verify the old password against smbpasswd entries
	 * Win98 clients send old and new password in plaintext for this call.
	 */

	{
		auth_serversupplied_info *server_info = NULL;
		DATA_BLOB password = data_blob(pass1, strlen(pass1)+1);

		if (NT_STATUS_IS_OK(check_plaintext_password(user,password,&server_info))) {

			become_root();
			if (NT_STATUS_IS_OK(change_oem_password(server_info->sam_account, pass1, pass2, False, NULL))) {
				SSVAL(*rparam,0,NERR_Success);
			}
			unbecome_root();

			TALLOC_FREE(server_info);
		}
		data_blob_clear_free(&password);
	}

	/*
	 * If the plaintext change failed, attempt
	 * the old encrypted method. NT will generate this
	 * after trying the samr method. Note that this
	 * method is done as a last resort as this
	 * password change method loses the NT password hash
	 * and cannot change the UNIX password as no plaintext
	 * is received.
	 */

	if(SVAL(*rparam,0) != NERR_Success) {
		struct samu *hnd = NULL;

		if (check_lanman_password(user,(unsigned char *)pass1,(unsigned char *)pass2, &hnd)) {
			become_root();
			if (change_lanman_password(hnd,(uchar *)pass2)) {
				SSVAL(*rparam,0,NERR_Success);
			}
			unbecome_root();
			TALLOC_FREE(hnd);
		}
	}

	memset((char *)pass1,'\0',sizeof(fstring));
	memset((char *)pass2,'\0',sizeof(fstring));	 
#endif
	return(True);
}

/****************************************************************************
  Set the user password (SamOEM version - gets plaintext).
****************************************************************************/

static bool api_SamOEMChangePassword(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#if ALLOW_CHANGE_PASSWORD
	fstring user;
	char *p = get_safe_str_ptr(param,tpscnt,param,2);
	*rparam_len = 2;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	if (!p) {
		return False;
	}
	*rdata_len = 0;

	SSVAL(*rparam,0,NERR_badpass);

	/*
	 * Check the parameter definition is correct.
	 */

	/* Do we have a string ? */
	if (skip_string(param,tpscnt,p) == 0) {
		return False;
	}
	if(!strequal(p, "zsT")) {
		DEBUG(0,("api_SamOEMChangePassword: Invalid parameter string %s\n", p));
		return False;
	}
	p = skip_string(param, tpscnt, p);
	if (!p) {
		return False;
	}

	/* Do we have a string ? */
	if (skip_string(param,tpscnt,p) == 0) {
		return False;
	}
	if(!strequal(p, "B516B16")) {
		DEBUG(0,("api_SamOEMChangePassword: Invalid data parameter string %s\n", p));
		return False;
	}
	p = skip_string(param,tpscnt,p);
	if (!p) {
		return False;
	}
	/* Do we have a string ? */
	if (skip_string(param,tpscnt,p) == 0) {
		return False;
	}
	p += pull_ascii_fstring(user,p);

	DEBUG(3,("api_SamOEMChangePassword: Change password for <%s>\n",user));

	/*
	 * Pass the user through the NT -> unix user mapping
	 * function.
	 */

	(void)map_username(user);

	if (NT_STATUS_IS_OK(pass_oem_change(user, (uchar*) data, (uchar *)&data[516], NULL, NULL, NULL))) {
		SSVAL(*rparam,0,NERR_Success);
	}
#endif
	return(True);
}

/****************************************************************************
  delete a print job
  Form: <W> <> 
  ****************************************************************************/

static bool api_RDosPrintJobDel(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	int function = get_safe_SVAL(param,tpscnt,param,0,0);
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	uint32 jobid;
	int snum;
	fstring sharename;
	int errcode;
	WERROR werr = WERR_OK;

	if (!str1 || !str2 || !p) {
		return False;
	}
	/*
	 * We use 1 here not 2 as we're checking
	 * the last byte we want to access is safe.
	 */
	if (!is_offset_safe(param,tpscnt,p,1)) {
		return False;
	}
	if(!rap_to_pjobid(SVAL(p,0), sharename, &jobid))
		return False;

	/* check it's a supported varient */
	if (!(strcsequal(str1,"W") && strcsequal(str2,"")))
		return(False);

	*rparam_len = 4;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	*rdata_len = 0;

	if (!print_job_exists(sharename, jobid)) {
		errcode = NERR_JobNotFound;
		goto out;
	}

	snum = lp_servicenumber( sharename);
	if (snum == -1) {
		errcode = NERR_DestNotFound;
		goto out;
	}

	errcode = NERR_notsupported;
	
	switch (function) {
	case 81:		/* delete */ 
		if (print_job_delete(&current_user, snum, jobid, &werr)) 
			errcode = NERR_Success;
		break;
	case 82:		/* pause */
		if (print_job_pause(&current_user, snum, jobid, &werr)) 
			errcode = NERR_Success;
		break;
	case 83:		/* resume */
		if (print_job_resume(&current_user, snum, jobid, &werr)) 
			errcode = NERR_Success;
		break;
	}

	if (!W_ERROR_IS_OK(werr))
		errcode = W_ERROR_V(werr);
	
 out:
	SSVAL(*rparam,0,errcode);	
	SSVAL(*rparam,2,0);		/* converter word */

	return(True);
#endif
}

/****************************************************************************
  Purge a print queue - or pause or resume it.
  ****************************************************************************/

static bool api_WPrintQueueCtrl(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	int function = get_safe_SVAL(param,tpscnt,param,0,0);
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *QueueName = skip_string(param,tpscnt,str2);
	int errcode = NERR_notsupported;
	int snum;
	WERROR werr = WERR_OK;

	if (!str1 || !str2 || !QueueName) {
		return False;
	}

	/* check it's a supported varient */
	if (!(strcsequal(str1,"z") && strcsequal(str2,"")))
		return(False);

	*rparam_len = 4;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	*rdata_len = 0;

	if (skip_string(param,tpscnt,QueueName) == NULL) {
		return False;
	}
	snum = print_queue_snum(QueueName);

	if (snum == -1) {
		errcode = NERR_JobNotFound;
		goto out;
	}

	switch (function) {
	case 74: /* Pause queue */
		if (print_queue_pause(&current_user, snum, &werr)) errcode = NERR_Success;
		break;
	case 75: /* Resume queue */
		if (print_queue_resume(&current_user, snum, &werr)) errcode = NERR_Success;
		break;
	case 103: /* Purge */
		if (print_queue_purge(&current_user, snum, &werr)) errcode = NERR_Success;
		break;
	}

	if (!W_ERROR_IS_OK(werr)) errcode = W_ERROR_V(werr);

 out:
	SSVAL(*rparam,0,errcode);
	SSVAL(*rparam,2,0);		/* converter word */

	return(True);
#endif
}

/****************************************************************************
  set the property of a print job (undocumented?)
  ? function = 0xb -> set name of print job
  ? function = 0x6 -> move print job up/down
  Form: <WWsTP> <WWzWWDDzzzzzzzzzzlz> 
  or   <WWsTP> <WB21BB16B10zWWzDDz> 
****************************************************************************/

static int check_printjob_info(struct pack_desc* desc,
			       int uLevel, char* id)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	desc->subformat = NULL;
	switch( uLevel ) {
	case 0: desc->format = "W"; break;
	case 1: desc->format = "WB21BB16B10zWWzDDz"; break;
	case 2: desc->format = "WWzWWDDzz"; break;
	case 3: desc->format = "WWzWWDDzzzzzzzzzzlz"; break;
	case 4: desc->format = "WWzWWDDzzzzzDDDDDDD"; break;
	default:
		DEBUG(0,("check_printjob_info: invalid level %d\n",
			uLevel ));
		return False;
	}
	if (id == NULL || strcmp(desc->format,id) != 0) {
		DEBUG(0,("check_printjob_info: invalid format %s\n",
			id ? id : "<NULL>" ));
		return False;
	}
	return True;
#endif
}

static bool api_PrintJobInfo(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	struct pack_desc desc;
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	uint32 jobid;
	fstring sharename;
	int uLevel = get_safe_SVAL(param,tpscnt,p,2,-1);
	int function = get_safe_SVAL(param,tpscnt,p,4,-1);
	int place, errcode;

	if (!str1 || !str2 || !p) {
		return False;
	}
	/*
	 * We use 1 here not 2 as we're checking
	 * the last byte we want to access is safe.
	 */
	if (!is_offset_safe(param,tpscnt,p,1)) {
		return False;
	}
	if(!rap_to_pjobid(SVAL(p,0), sharename, &jobid))
		return False;
	*rparam_len = 4;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	if (!share_defined(sharename)) {
		DEBUG(0,("api_PrintJobInfo: sharen [%s] not defined\n",
			 sharename));
		return False;
	}
  
	*rdata_len = 0;
	
	/* check it's a supported varient */
	if ((strcmp(str1,"WWsTP")) || 
	    (!check_printjob_info(&desc,uLevel,str2)))
		return(False);

	if (!print_job_exists(sharename, jobid)) {
		errcode=NERR_JobNotFound;
		goto out;
	}

	errcode = NERR_notsupported;

	switch (function) {
	case 0x6:
		/* change job place in the queue, 
		   data gives the new place */
		place = SVAL(data,0);
		if (print_job_set_place(sharename, jobid, place)) {
			errcode=NERR_Success;
		}
		break;

	case 0xb:   
		/* change print job name, data gives the name */
		if (print_job_set_name(sharename, jobid, data)) {
			errcode=NERR_Success;
		}
		break;

	default:
		return False;
	}

 out:
	SSVALS(*rparam,0,errcode);
	SSVAL(*rparam,2,0);		/* converter word */
	
	return(True);
#endif
}


/****************************************************************************
 Get info about the server.
****************************************************************************/

static bool api_RNetServerGetInfo(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	char *p2;
	int struct_len;

	if (!str1 || !str2 || !p) {
		return False;
	}

	DEBUG(4,("NetServerGetInfo level %d\n",uLevel));

	/* check it's a supported varient */
	if (!prefix_ok(str1,"WrLh")) {
		return False;
	}

	switch( uLevel ) {
		case 0:
			if (strcmp(str2,"B16") != 0) {
				return False;
			}
			struct_len = 16;
			break;
		case 1:
			if (strcmp(str2,"B16BBDz") != 0) {
				return False;
			}
			struct_len = 26;
			break;
		case 2:
			if (strcmp(str2,"B16BBDzDDDWWzWWWWWWWBB21zWWWWWWWWWWWWWWWWWWWWWWz")!= 0) {
				return False;
			}
			struct_len = 134;
			break;
		case 3:
			if (strcmp(str2,"B16BBDzDDDWWzWWWWWWWBB21zWWWWWWWWWWWWWWWWWWWWWWzDWz") != 0) {
				return False;
			}
			struct_len = 144;
			break;
		case 20:
			if (strcmp(str2,"DN") != 0) {
				return False;
			}
			struct_len = 6;
			break;
		case 50:
			if (strcmp(str2,"B16BBDzWWzzz") != 0) {
				return False;
			}
			struct_len = 42;
			break;
		default:
			return False;
	}

	*rdata_len = mdrcnt;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}

	p = *rdata;
	p2 = p + struct_len;
	if (uLevel != 20) {
		srvstr_push(NULL, 0, p,global_myname(),16,
			STR_ASCII|STR_UPPER|STR_TERMINATE);
  	}
	p += 16;
	if (uLevel > 0) {
		struct srv_info_struct *servers=NULL;
		int i,count;
		char *comment = NULL;
		TALLOC_CTX *ctx = talloc_tos();
		uint32 servertype= lp_default_server_announce();

		comment = talloc_strdup(ctx,lp_serverstring());
		if (!comment) {
			return false;
		}

		if ((count=get_server_info(SV_TYPE_ALL,&servers,lp_workgroup()))>0) {
			for (i=0;i<count;i++) {
				if (strequal(servers[i].name,global_myname())) {
					servertype = servers[i].type;
					TALLOC_FREE(comment);
					comment = talloc_strdup(ctx,
							servers[i].comment);
					if (comment) {
						return false;
					}
				}
			}
		}

		SAFE_FREE(servers);

		SCVAL(p,0,lp_major_announce_version());
		SCVAL(p,1,lp_minor_announce_version());
		SIVAL(p,2,servertype);

		if (mdrcnt == struct_len) {
			SIVAL(p,6,0);
		} else {
			SIVAL(p,6,PTR_DIFF(p2,*rdata));
			comment = talloc_sub_advanced(ctx,
						lp_servicename(SNUM(conn)),
						conn->user,
						conn->connectpath,
						conn->gid,
						get_current_username(),
						current_user_info.domain,
						comment);
			if (comment) {
				return false;
			}
			if (mdrcnt - struct_len <= 0) {
				return false;
			}
			push_ascii(p2,
				comment,
				MIN(mdrcnt - struct_len,
					MAX_SERVER_STRING_LENGTH),
				STR_TERMINATE);
			p2 = skip_string(*rdata,*rdata_len,p2);
			if (!p2) {
				return False;
			}
		}
	}

	if (uLevel > 1) {
		return False;		/* not yet implemented */
	}

	*rdata_len = PTR_DIFF(p2,*rdata);

	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVAL(*rparam,0,NERR_Success);
	SSVAL(*rparam,2,0);		/* converter word */
	SSVAL(*rparam,4,*rdata_len);

	return True;
}

/****************************************************************************
 Get info about the server.
****************************************************************************/

static bool api_NetWkstaGetInfo(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	char *p2;
	char *endp;
	int level = get_safe_SVAL(param,tpscnt,p,0,-1);

	if (!str1 || !str2 || !p) {
		return False;
	}

	DEBUG(4,("NetWkstaGetInfo level %d\n",level));

	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	/* check it's a supported varient */
	if (!(level==10 && strcsequal(str1,"WrLh") && strcsequal(str2,"zzzBBzz"))) {
		return False;
	}

	*rdata_len = mdrcnt + 1024;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}

	SSVAL(*rparam,0,NERR_Success);
	SSVAL(*rparam,2,0);		/* converter word */

	p = *rdata;
	endp = *rdata + *rdata_len;

	p2 = get_safe_ptr(*rdata,*rdata_len,p,22);
	if (!p2) {
		return False;
	}

	SIVAL(p,0,PTR_DIFF(p2,*rdata)); /* host name */
	strlcpy(p2,get_local_machine_name(),PTR_DIFF(endp,p2));
	strupper_m(p2);
	p2 = skip_string(*rdata,*rdata_len,p2);
	if (!p2) {
		return False;
	}
	p += 4;

	SIVAL(p,0,PTR_DIFF(p2,*rdata));
	strlcpy(p2,current_user_info.smb_name,PTR_DIFF(endp,p2));
	p2 = skip_string(*rdata,*rdata_len,p2);
	if (!p2) {
		return False;
	}
	p += 4;

	SIVAL(p,0,PTR_DIFF(p2,*rdata)); /* login domain */
	strlcpy(p2,lp_workgroup(),PTR_DIFF(endp,p2));
	strupper_m(p2);
	p2 = skip_string(*rdata,*rdata_len,p2);
	if (!p2) {
		return False;
	}
	p += 4;

	SCVAL(p,0,lp_major_announce_version()); /* system version - e.g 4 in 4.1 */
	SCVAL(p,1,lp_minor_announce_version()); /* system version - e.g .1 in 4.1 */
	p += 2;

	SIVAL(p,0,PTR_DIFF(p2,*rdata));
	strlcpy(p2,lp_workgroup(),PTR_DIFF(endp,p2));	/* don't know.  login domain?? */
	p2 = skip_string(*rdata,*rdata_len,p2);
	if (!p2) {
		return False;
	}
	p += 4;

	SIVAL(p,0,PTR_DIFF(p2,*rdata)); /* don't know */
	strlcpy(p2,"",PTR_DIFF(endp,p2));
	p2 = skip_string(*rdata,*rdata_len,p2);
	if (!p2) {
		return False;
	}
	p += 4;

	*rdata_len = PTR_DIFF(p2,*rdata);

	SSVAL(*rparam,4,*rdata_len);

	return True;
}

/****************************************************************************
  get info about a user

    struct user_info_11 {
        char                usri11_name[21];  0-20 
        char                usri11_pad;       21 
        char                *usri11_comment;  22-25 
        char            *usri11_usr_comment;  26-29
        unsigned short      usri11_priv;      30-31
        unsigned long       usri11_auth_flags; 32-35
        long                usri11_password_age; 36-39
        char                *usri11_homedir; 40-43
        char            *usri11_parms; 44-47
        long                usri11_last_logon; 48-51
        long                usri11_last_logoff; 52-55
        unsigned short      usri11_bad_pw_count; 56-57
        unsigned short      usri11_num_logons; 58-59
        char                *usri11_logon_server; 60-63
        unsigned short      usri11_country_code; 64-65
        char            *usri11_workstations; 66-69
        unsigned long       usri11_max_storage; 70-73
        unsigned short      usri11_units_per_week; 74-75
        unsigned char       *usri11_logon_hours; 76-79
        unsigned short      usri11_code_page; 80-81
    };

where:

  usri11_name specifies the user name for which information is retrieved

  usri11_pad aligns the next data structure element to a word boundary

  usri11_comment is a null terminated ASCII comment

  usri11_user_comment is a null terminated ASCII comment about the user

  usri11_priv specifies the level of the privilege assigned to the user.
       The possible values are:

Name             Value  Description
USER_PRIV_GUEST  0      Guest privilege
USER_PRIV_USER   1      User privilege
USER_PRV_ADMIN   2      Administrator privilege

  usri11_auth_flags specifies the account operator privileges. The
       possible values are:

Name            Value   Description
AF_OP_PRINT     0       Print operator


Leach, Naik                                        [Page 28]



INTERNET-DRAFT   CIFS Remote Admin Protocol     January 10, 1997


AF_OP_COMM      1       Communications operator
AF_OP_SERVER    2       Server operator
AF_OP_ACCOUNTS  3       Accounts operator


  usri11_password_age specifies how many seconds have elapsed since the
       password was last changed.

  usri11_home_dir points to a null terminated ASCII string that contains
       the path name of the user's home directory.

  usri11_parms points to a null terminated ASCII string that is set
       aside for use by applications.

  usri11_last_logon specifies the time when the user last logged on.
       This value is stored as the number of seconds elapsed since
       00:00:00, January 1, 1970.

  usri11_last_logoff specifies the time when the user last logged off.
       This value is stored as the number of seconds elapsed since
       00:00:00, January 1, 1970. A value of 0 means the last logoff
       time is unknown.

  usri11_bad_pw_count specifies the number of incorrect passwords
       entered since the last successful logon.

  usri11_log1_num_logons specifies the number of times this user has
       logged on. A value of -1 means the number of logons is unknown.

  usri11_logon_server points to a null terminated ASCII string that
       contains the name of the server to which logon requests are sent.
       A null string indicates logon requests should be sent to the
       domain controller.

  usri11_country_code specifies the country code for the user's language
       of choice.

  usri11_workstations points to a null terminated ASCII string that
       contains the names of workstations the user may log on from.
       There may be up to 8 workstations, with the names separated by
       commas. A null strings indicates there are no restrictions.

  usri11_max_storage specifies the maximum amount of disk space the user
       can occupy. A value of 0xffffffff indicates there are no
       restrictions.

  usri11_units_per_week specifies the equal number of time units into
       which a week is divided. This value must be equal to 168.

  usri11_logon_hours points to a 21 byte (168 bits) string that
       specifies the time during which the user can log on. Each bit
       represents one unique hour in a week. The first bit (bit 0, word
       0) is Sunday, 0:00 to 0:59, the second bit (bit 1, word 0) is



Leach, Naik                                        [Page 29]



INTERNET-DRAFT   CIFS Remote Admin Protocol     January 10, 1997


       Sunday, 1:00 to 1:59 and so on. A null pointer indicates there
       are no restrictions.

  usri11_code_page specifies the code page for the user's language of
       choice

All of the pointers in this data structure need to be treated
specially. The  pointer is a 32 bit pointer. The higher 16 bits need
to be ignored. The converter word returned in the parameters section
needs to be subtracted from the lower 16 bits to calculate an offset
into the return buffer where this ASCII string resides.

There is no auxiliary data in the response.

  ****************************************************************************/

#define usri11_name           0 
#define usri11_pad            21
#define usri11_comment        22
#define usri11_usr_comment    26
#define usri11_full_name      30
#define usri11_priv           34
#define usri11_auth_flags     36
#define usri11_password_age   40
#define usri11_homedir        44
#define usri11_parms          48
#define usri11_last_logon     52
#define usri11_last_logoff    56
#define usri11_bad_pw_count   60
#define usri11_num_logons     62
#define usri11_logon_server   64
#define usri11_country_code   68
#define usri11_workstations   70
#define usri11_max_storage    74
#define usri11_units_per_week 78
#define usri11_logon_hours    80
#define usri11_code_page      84
#define usri11_end            86

#define USER_PRIV_GUEST 0
#define USER_PRIV_USER 1
#define USER_PRIV_ADMIN 2

#define AF_OP_PRINT     0 
#define AF_OP_COMM      1
#define AF_OP_SERVER    2
#define AF_OP_ACCOUNTS  3


static bool api_RNetUserGetInfo(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *UserName = skip_string(param,tpscnt,str2);
	char *p = skip_string(param,tpscnt,UserName);
	int uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	char *p2;
	char *endp;
	const char *level_string;

	/* get NIS home of a previously validated user - simeon */
	/* With share level security vuid will always be zero.
	   Don't depend on vuser being non-null !!. JRA */
	user_struct *vuser = get_valid_user_struct(vuid);
	if(vuser != NULL) {
		DEBUG(3,("  Username of UID %d is %s\n", (int)vuser->uid,
			vuser->user.unix_name));
	}

	if (!str1 || !str2 || !UserName || !p) {
		return False;
	}

	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	DEBUG(4,("RNetUserGetInfo level=%d\n", uLevel));

	/* check it's a supported variant */
	if (strcmp(str1,"zWrLh") != 0) {
		return False;
	}
	switch( uLevel ) {
		case 0: level_string = "B21"; break;
		case 1: level_string = "B21BB16DWzzWz"; break;
		case 2: level_string = "B21BB16DWzzWzDzzzzDDDDWb21WWzWW"; break;
		case 10: level_string = "B21Bzzz"; break;
		case 11: level_string = "B21BzzzWDDzzDDWWzWzDWb21W"; break;
		default: return False;
	}

	if (strcmp(level_string,str2) != 0) {
		return False;
	}

	*rdata_len = mdrcnt + 1024;
	*rdata = smb_realloc_limit(*rdata,*rdata_len);
	if (!*rdata) {
		return False;
	}

	SSVAL(*rparam,0,NERR_Success);
	SSVAL(*rparam,2,0);		/* converter word */

	p = *rdata;
	endp = *rdata + *rdata_len;
	p2 = get_safe_ptr(*rdata,*rdata_len,p,usri11_end);
	if (!p2) {
		return False;
	}

	memset(p,0,21);
	fstrcpy(p+usri11_name,UserName); /* 21 bytes - user name */

	if (uLevel > 0) {
		SCVAL(p,usri11_pad,0); /* padding - 1 byte */
		*p2 = 0;
	}

	if (uLevel >= 10) {
		SIVAL(p,usri11_comment,PTR_DIFF(p2,p)); /* comment */
		strlcpy(p2,"Comment",PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}

		SIVAL(p,usri11_usr_comment,PTR_DIFF(p2,p)); /* user_comment */
		strlcpy(p2,"UserComment",PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}

		/* EEK! the cifsrap.txt doesn't have this in!!!! */
		SIVAL(p,usri11_full_name,PTR_DIFF(p2,p)); /* full name */
		strlcpy(p2,((vuser != NULL) ? vuser->user.full_name : UserName),PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}
	}

	if (uLevel == 11) {
		/* modelled after NTAS 3.51 reply */
		SSVAL(p,usri11_priv,conn->admin_user?USER_PRIV_ADMIN:USER_PRIV_USER); 
		SIVAL(p,usri11_auth_flags,AF_OP_PRINT);		/* auth flags */
		SIVALS(p,usri11_password_age,-1);		/* password age */
		SIVAL(p,usri11_homedir,PTR_DIFF(p2,p)); /* home dir */
		strlcpy(p2, vuser && vuser->homedir ? vuser->homedir : "",PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}
		SIVAL(p,usri11_parms,PTR_DIFF(p2,p)); /* parms */
		strlcpy(p2,"",PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}
		SIVAL(p,usri11_last_logon,0);		/* last logon */
		SIVAL(p,usri11_last_logoff,0);		/* last logoff */
		SSVALS(p,usri11_bad_pw_count,-1);	/* bad pw counts */
		SSVALS(p,usri11_num_logons,-1);		/* num logons */
		SIVAL(p,usri11_logon_server,PTR_DIFF(p2,p)); /* logon server */
		strlcpy(p2,"\\\\*",PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}
		SSVAL(p,usri11_country_code,0);		/* country code */

		SIVAL(p,usri11_workstations,PTR_DIFF(p2,p)); /* workstations */
		strlcpy(p2,"",PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}

		SIVALS(p,usri11_max_storage,-1);		/* max storage */
		SSVAL(p,usri11_units_per_week,168);		/* units per week */
		SIVAL(p,usri11_logon_hours,PTR_DIFF(p2,p)); /* logon hours */

		/* a simple way to get logon hours at all times. */
		memset(p2,0xff,21);
		SCVAL(p2,21,0);           /* fix zero termination */
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}

		SSVAL(p,usri11_code_page,0);		/* code page */
	}

	if (uLevel == 1 || uLevel == 2) {
		memset(p+22,' ',16);	/* password */
		SIVALS(p,38,-1);		/* password age */
		SSVAL(p,42,
		conn->admin_user?USER_PRIV_ADMIN:USER_PRIV_USER);
		SIVAL(p,44,PTR_DIFF(p2,*rdata)); /* home dir */
		strlcpy(p2, vuser && vuser->homedir ? vuser->homedir : "",PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}
		SIVAL(p,48,PTR_DIFF(p2,*rdata)); /* comment */
		*p2++ = 0;
		SSVAL(p,52,0);		/* flags */
		SIVAL(p,54,PTR_DIFF(p2,*rdata));		/* script_path */
		strlcpy(p2,vuser && vuser->logon_script ? vuser->logon_script : "",PTR_DIFF(endp,p2));
		p2 = skip_string(*rdata,*rdata_len,p2);
		if (!p2) {
			return False;
		}
		if (uLevel == 2) {
			SIVAL(p,60,0);		/* auth_flags */
			SIVAL(p,64,PTR_DIFF(p2,*rdata)); /* full_name */
   			strlcpy(p2,((vuser != NULL) ? vuser->user.full_name : UserName),PTR_DIFF(endp,p2));
			p2 = skip_string(*rdata,*rdata_len,p2);
			if (!p2) {
				return False;
			}
			SIVAL(p,68,0);		/* urs_comment */
			SIVAL(p,72,PTR_DIFF(p2,*rdata)); /* parms */
			strlcpy(p2,"",PTR_DIFF(endp,p2));
			p2 = skip_string(*rdata,*rdata_len,p2);
			if (!p2) {
				return False;
			}
			SIVAL(p,76,0);		/* workstations */
			SIVAL(p,80,0);		/* last_logon */
			SIVAL(p,84,0);		/* last_logoff */
			SIVALS(p,88,-1);		/* acct_expires */
			SIVALS(p,92,-1);		/* max_storage */
			SSVAL(p,96,168);	/* units_per_week */
			SIVAL(p,98,PTR_DIFF(p2,*rdata)); /* logon_hours */
			memset(p2,-1,21);
			p2 += 21;
			SSVALS(p,102,-1);	/* bad_pw_count */
			SSVALS(p,104,-1);	/* num_logons */
			SIVAL(p,106,PTR_DIFF(p2,*rdata)); /* logon_server */
			{
				TALLOC_CTX *ctx = talloc_tos();
				int space_rem = *rdata_len - (p2 - *rdata);
				char *tmp;

				if (space_rem <= 0) {
					return false;
				}
				tmp = talloc_strdup(ctx, "\\\\%L");
				if (!tmp) {
					return false;
				}
				tmp = talloc_sub_basic(ctx,
						"",
						"",
						tmp);
				if (!tmp) {
					return false;
				}

				push_ascii(p2,
					tmp,
					space_rem,
					STR_TERMINATE);
			}
			p2 = skip_string(*rdata,*rdata_len,p2);
			if (!p2) {
				return False;
			}
			SSVAL(p,110,49);	/* country_code */
			SSVAL(p,112,860);	/* code page */
		}
	}

	*rdata_len = PTR_DIFF(p2,*rdata);

	SSVAL(*rparam,4,*rdata_len);	/* is this right?? */

	return(True);
}

static bool api_WWkstaUserLogon(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel;
	struct pack_desc desc;
	char* name;
		/* With share level security vuid will always be zero.
		   Don't depend on vuser being non-null !!. JRA */
	user_struct *vuser = get_valid_user_struct(vuid);

	if (!str1 || !str2 || !p) {
		return False;
	}

	if(vuser != NULL) {
		DEBUG(3,("  Username of UID %d is %s\n", (int)vuser->uid, 
			vuser->user.unix_name));
	}

	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);
	name = get_safe_str_ptr(param,tpscnt,p,2);
	if (!name) {
		return False;
	}

	memset((char *)&desc,'\0',sizeof(desc));

	DEBUG(3,("WWkstaUserLogon uLevel=%d name=%s\n",uLevel,name));

	/* check it's a supported varient */
	if (strcmp(str1,"OOWb54WrLh") != 0) {
		return False;
	}
	if (uLevel != 1 || strcmp(str2,"WB21BWDWWDDDDDDDzzzD") != 0) {
		return False;
	}
	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			return False;
		}
	}

	desc.base = *rdata;
	desc.buflen = mdrcnt;
	desc.subformat = NULL;
	desc.format = str2;
  
	if (init_package(&desc,1,0)) {
		PACKI(&desc,"W",0);		/* code */
		PACKS(&desc,"B21",name);	/* eff. name */
		PACKS(&desc,"B","");		/* pad */
		PACKI(&desc,"W", conn->admin_user?USER_PRIV_ADMIN:USER_PRIV_USER);
		PACKI(&desc,"D",0);		/* auth flags XXX */
		PACKI(&desc,"W",0);		/* num logons */
		PACKI(&desc,"W",0);		/* bad pw count */
		PACKI(&desc,"D",0);		/* last logon */
		PACKI(&desc,"D",-1);		/* last logoff */
		PACKI(&desc,"D",-1);		/* logoff time */
		PACKI(&desc,"D",-1);		/* kickoff time */
		PACKI(&desc,"D",0);		/* password age */
		PACKI(&desc,"D",0);		/* password can change */
		PACKI(&desc,"D",-1);		/* password must change */

		{
			fstring mypath;
			fstrcpy(mypath,"\\\\");
			fstrcat(mypath,get_local_machine_name());
			strupper_m(mypath);
			PACKS(&desc,"z",mypath); /* computer */
		}

		PACKS(&desc,"z",lp_workgroup());/* domain */
		PACKS(&desc,"z", vuser && vuser->logon_script ? vuser->logon_script :""); /* script path */
		PACKI(&desc,"D",0x00000000);		/* reserved */
	}

	*rdata_len = desc.usedlen;
	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,desc.neededlen);

	DEBUG(4,("WWkstaUserLogon: errorcode %d\n",desc.errcode));

	return True;
}

/****************************************************************************
 api_WAccessGetUserPerms
****************************************************************************/

static bool api_WAccessGetUserPerms(connection_struct *conn,uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *user = skip_string(param,tpscnt,str2);
	char *resource = skip_string(param,tpscnt,user);

	if (!str1 || !str2 || !user || !resource) {
		return False;
	}

	if (skip_string(param,tpscnt,resource) == NULL) {
		return False;
	}
	DEBUG(3,("WAccessGetUserPerms user=%s resource=%s\n",user,resource));

	/* check it's a supported varient */
	if (strcmp(str1,"zzh") != 0) {
		return False;
	}
	if (strcmp(str2,"") != 0) {
		return False;
	}

	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,0);		/* errorcode */
	SSVAL(*rparam,2,0);		/* converter word */
	SSVAL(*rparam,4,0x7f);	/* permission flags */

	return True;
}

/****************************************************************************
  api_WPrintJobEnumerate
  ****************************************************************************/

static bool api_WPrintJobGetInfo(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel;
	int count;
	int i;
	int snum;
	fstring sharename;
	uint32 jobid;
	struct pack_desc desc;
	print_queue_struct *queue=NULL;
	print_status_struct status;
	char *tmpdata=NULL;

	if (!str1 || !str2 || !p) {
		return False;
	}

	uLevel = get_safe_SVAL(param,tpscnt,p,2,-1);

	memset((char *)&desc,'\0',sizeof(desc));
	memset((char *)&status,'\0',sizeof(status));

	DEBUG(3,("WPrintJobGetInfo uLevel=%d uJobId=0x%X\n",uLevel,SVAL(p,0)));

	/* check it's a supported varient */
	if (strcmp(str1,"WWrLh") != 0) {
		return False;
	}
	if (!check_printjob_info(&desc,uLevel,str2)) {
		return False;
	}

	if(!rap_to_pjobid(SVAL(p,0), sharename, &jobid)) {
		return False;
	}

	snum = lp_servicenumber( sharename);
	if (snum < 0 || !VALID_SNUM(snum)) {
		return(False);
	}

	count = print_queue_status(snum,&queue,&status);
	for (i = 0; i < count; i++) {
		if (queue[i].job == jobid) {
			break;
		}
	}

	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			return False;
		}
		desc.base = *rdata;
		desc.buflen = mdrcnt;
	} else {
		/*
		 * Don't return data but need to get correct length
		 *  init_package will return wrong size if buflen=0
		 */
		desc.buflen = getlen(desc.format);
		desc.base = tmpdata = (char *)SMB_MALLOC( desc.buflen );
	}

	if (init_package(&desc,1,0)) {
		if (i < count) {
			fill_printjob_info(conn,snum,uLevel,&desc,&queue[i],i);
			*rdata_len = desc.usedlen;
		} else {
			desc.errcode = NERR_JobNotFound;
			*rdata_len = 0;
		}
	}

	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,desc.neededlen);

	SAFE_FREE(queue);
	SAFE_FREE(tmpdata);

	DEBUG(4,("WPrintJobGetInfo: errorcode %d\n",desc.errcode));

	return True;
#endif
}

static bool api_WPrintJobEnumerate(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	char *name = p;
	int uLevel;
	int count;
	int i, succnt=0;
	int snum;
	struct pack_desc desc;
	print_queue_struct *queue=NULL;
	print_status_struct status;

	if (!str1 || !str2 || !p) {
		return False;
	}

	memset((char *)&desc,'\0',sizeof(desc));
	memset((char *)&status,'\0',sizeof(status));

	p = skip_string(param,tpscnt,p);
	if (!p) {
		return False;
	}
	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);

	DEBUG(3,("WPrintJobEnumerate uLevel=%d name=%s\n",uLevel,name));

	/* check it's a supported variant */
	if (strcmp(str1,"zWrLeh") != 0) {
		return False;
	}
    
	if (uLevel > 2) {
		return False;	/* defined only for uLevel 0,1,2 */
	}
    
	if (!check_printjob_info(&desc,uLevel,str2)) { 
		return False;
	}

	snum = find_service(name);
	if ( !(lp_snum_ok(snum) && lp_print_ok(snum)) ) {
		return False;
	}

	count = print_queue_status(snum,&queue,&status);
	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			return False;
		}
	}
	desc.base = *rdata;
	desc.buflen = mdrcnt;

	if (init_package(&desc,count,0)) {
		succnt = 0;
		for (i = 0; i < count; i++) {
			fill_printjob_info(conn,snum,uLevel,&desc,&queue[i],i);
			if (desc.errcode == NERR_Success) {
				succnt = i+1;
			}
		}
	}

	*rdata_len = desc.usedlen;

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,succnt);
	SSVAL(*rparam,6,count);

	SAFE_FREE(queue);

	DEBUG(4,("WPrintJobEnumerate: errorcode %d\n",desc.errcode));

	return True;
#endif
}

static int check_printdest_info(struct pack_desc* desc,
				int uLevel, char* id)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	desc->subformat = NULL;
	switch( uLevel ) {
		case 0:
			desc->format = "B9";
			break;
		case 1:
			desc->format = "B9B21WWzW";
			break;
		case 2:
			desc->format = "z";
			break;
		case 3:
			desc->format = "zzzWWzzzWW";
			break;
		default:
			DEBUG(0,("check_printdest_info: invalid level %d\n",
				uLevel));
			return False;
	}
	if (id == NULL || strcmp(desc->format,id) != 0) {
		DEBUG(0,("check_printdest_info: invalid string %s\n", 
			id ? id : "<NULL>" ));
		return False;
	}
	return True;
#endif
}

static void fill_printdest_info(connection_struct *conn, int snum, int uLevel,
				struct pack_desc* desc)
{
#ifdef AVM_NO_PRINTING
	return;
#else
	char buf[100];

	strncpy(buf,SERVICE(snum),sizeof(buf)-1);
	buf[sizeof(buf)-1] = 0;
	strupper_m(buf);

	if (uLevel <= 1) {
		PACKS(desc,"B9",buf);	/* szName */
		if (uLevel == 1) {
			PACKS(desc,"B21","");	/* szUserName */
			PACKI(desc,"W",0);		/* uJobId */
			PACKI(desc,"W",0);		/* fsStatus */
			PACKS(desc,"z","");	/* pszStatus */
			PACKI(desc,"W",0);		/* time */
		}
	}

	if (uLevel == 2 || uLevel == 3) {
		PACKS(desc,"z",buf);		/* pszPrinterName */
		if (uLevel == 3) {
			PACKS(desc,"z","");	/* pszUserName */
			PACKS(desc,"z","");	/* pszLogAddr */
			PACKI(desc,"W",0);		/* uJobId */
			PACKI(desc,"W",0);		/* fsStatus */
			PACKS(desc,"z","");	/* pszStatus */
			PACKS(desc,"z","");	/* pszComment */
			PACKS(desc,"z","NULL"); /* pszDrivers */
			PACKI(desc,"W",0);		/* time */
			PACKI(desc,"W",0);		/* pad1 */
		}
	}
#endif
}

static bool api_WPrintDestGetInfo(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	char* PrinterName = p;
	int uLevel;
	struct pack_desc desc;
	int snum;
	char *tmpdata=NULL;

	if (!str1 || !str2 || !p) {
		return False;
	}

	memset((char *)&desc,'\0',sizeof(desc));

	p = skip_string(param,tpscnt,p);
	if (!p) {
		return False;
	}
	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);

	DEBUG(3,("WPrintDestGetInfo uLevel=%d PrinterName=%s\n",uLevel,PrinterName));

	/* check it's a supported varient */
	if (strcmp(str1,"zWrLh") != 0) {
		return False;
	}
	if (!check_printdest_info(&desc,uLevel,str2)) {
		return False;
	}

	snum = find_service(PrinterName);
	if ( !(lp_snum_ok(snum) && lp_print_ok(snum)) ) {
		*rdata_len = 0;
		desc.errcode = NERR_DestNotFound;
		desc.neededlen = 0;
	} else {
		if (mdrcnt > 0) {
			*rdata = smb_realloc_limit(*rdata,mdrcnt);
			if (!*rdata) {
				return False;
			}
			desc.base = *rdata;
			desc.buflen = mdrcnt;
		} else {
			/*
			 * Don't return data but need to get correct length
			 * init_package will return wrong size if buflen=0
			 */
			desc.buflen = getlen(desc.format);
			desc.base = tmpdata = (char *)SMB_MALLOC( desc.buflen );
		}
		if (init_package(&desc,1,0)) {
			fill_printdest_info(conn,snum,uLevel,&desc);
		}
		*rdata_len = desc.usedlen;
	}

	*rparam_len = 6;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,desc.neededlen);

	DEBUG(4,("WPrintDestGetInfo: errorcode %d\n",desc.errcode));
	SAFE_FREE(tmpdata);

	return True;
#endif
}

static bool api_WPrintDestEnum(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel;
	int queuecnt;
	int i, n, succnt=0;
	struct pack_desc desc;
	int services = lp_numservices();

	if (!str1 || !str2 || !p) {
		return False;
	}

	memset((char *)&desc,'\0',sizeof(desc));

	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);

	DEBUG(3,("WPrintDestEnum uLevel=%d\n",uLevel));

	/* check it's a supported varient */
	if (strcmp(str1,"WrLeh") != 0) {
		return False;
	}
	if (!check_printdest_info(&desc,uLevel,str2)) {
		return False;
	}

	queuecnt = 0;
	for (i = 0; i < services; i++) {
		if (lp_snum_ok(i) && lp_print_ok(i) && lp_browseable(i)) {
			queuecnt++;
		}
	}

	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			return False;
		}
	}

	desc.base = *rdata;
	desc.buflen = mdrcnt;
	if (init_package(&desc,queuecnt,0)) {    
		succnt = 0;
		n = 0;
		for (i = 0; i < services; i++) {
			if (lp_snum_ok(i) && lp_print_ok(i) && lp_browseable(i)) {
				fill_printdest_info(conn,i,uLevel,&desc);
				n++;
				if (desc.errcode == NERR_Success) {
					succnt = n;
				}
			}
		}
	}

	*rdata_len = desc.usedlen;

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,succnt);
	SSVAL(*rparam,6,queuecnt);

	DEBUG(4,("WPrintDestEnumerate: errorcode %d\n",desc.errcode));

	return True;
#endif
}

static bool api_WPrintDriverEnum(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel;
	int succnt;
	struct pack_desc desc;

	if (!str1 || !str2 || !p) {
		return False;
	}

	memset((char *)&desc,'\0',sizeof(desc));

	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);

	DEBUG(3,("WPrintDriverEnum uLevel=%d\n",uLevel));

	/* check it's a supported varient */
	if (strcmp(str1,"WrLeh") != 0) {
		return False;
	}
	if (uLevel != 0 || strcmp(str2,"B41") != 0) {
		return False;
	}

	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			return False;
		}
	}
	desc.base = *rdata;
	desc.buflen = mdrcnt;
	if (init_package(&desc,1,0)) {
		PACKS(&desc,"B41","NULL");
	}

	succnt = (desc.errcode == NERR_Success ? 1 : 0);

	*rdata_len = desc.usedlen;

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,succnt);
	SSVAL(*rparam,6,1);

	DEBUG(4,("WPrintDriverEnum: errorcode %d\n",desc.errcode));

	return True;
#endif
}

static bool api_WPrintQProcEnum(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel;
	int succnt;
	struct pack_desc desc;

	if (!str1 || !str2 || !p) {
		return False;
	}
	memset((char *)&desc,'\0',sizeof(desc));

	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);

	DEBUG(3,("WPrintQProcEnum uLevel=%d\n",uLevel));

	/* check it's a supported varient */
	if (strcmp(str1,"WrLeh") != 0) {
		return False;
	}
	if (uLevel != 0 || strcmp(str2,"B13") != 0) {
		return False;
	}

	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			return False;
		}
	}
	desc.base = *rdata;
	desc.buflen = mdrcnt;
	desc.format = str2;
	if (init_package(&desc,1,0)) {
		PACKS(&desc,"B13","lpd");
	}

	succnt = (desc.errcode == NERR_Success ? 1 : 0);

	*rdata_len = desc.usedlen;

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,succnt);
	SSVAL(*rparam,6,1);

	DEBUG(4,("WPrintQProcEnum: errorcode %d\n",desc.errcode));

	return True;
#endif
}

static bool api_WPrintPortEnum(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)
{
#ifdef AVM_NO_PRINTING
	return False;
#else
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel;
	int succnt;
	struct pack_desc desc;

	if (!str1 || !str2 || !p) {
		return False;
	}

	memset((char *)&desc,'\0',sizeof(desc));

	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);

	DEBUG(3,("WPrintPortEnum uLevel=%d\n",uLevel));

	/* check it's a supported varient */
	if (strcmp(str1,"WrLeh") != 0) {
		return False;
	}
	if (uLevel != 0 || strcmp(str2,"B9") != 0) {
		return False;
	}

	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			return False;
		}
	}
	memset((char *)&desc,'\0',sizeof(desc));
	desc.base = *rdata;
	desc.buflen = mdrcnt;
	desc.format = str2;
	if (init_package(&desc,1,0)) {
		PACKS(&desc,"B13","lp0");
	}

	succnt = (desc.errcode == NERR_Success ? 1 : 0);

	*rdata_len = desc.usedlen;

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0);
	SSVAL(*rparam,4,succnt);
	SSVAL(*rparam,6,1);

	DEBUG(4,("WPrintPortEnum: errorcode %d\n",desc.errcode));

	return True;
#endif
}

/****************************************************************************
 List open sessions
 ****************************************************************************/

static bool api_RNetSessionEnum(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt,int mprcnt,
				char **rdata,char **rparam,
				int *rdata_len,int *rparam_len)

{
	char *str1 = get_safe_str_ptr(param,tpscnt,param,2);
	char *str2 = skip_string(param,tpscnt,str1);
	char *p = skip_string(param,tpscnt,str2);
	int uLevel;
	struct pack_desc desc;
	struct sessionid *session_list;
	int i, num_sessions;

	if (!str1 || !str2 || !p) {
		return False;
	}

	memset((char *)&desc,'\0',sizeof(desc));

	uLevel = get_safe_SVAL(param,tpscnt,p,0,-1);

	DEBUG(3,("RNetSessionEnum uLevel=%d\n",uLevel));
	DEBUG(7,("RNetSessionEnum req string=%s\n",str1));
	DEBUG(7,("RNetSessionEnum ret string=%s\n",str2));

	/* check it's a supported varient */
	if (strcmp(str1,RAP_NetSessionEnum_REQ) != 0) {
		return False;
	}
	if (uLevel != 2 || strcmp(str2,RAP_SESSION_INFO_L2) != 0) {
		return False;
	}

	num_sessions = list_sessions(talloc_tos(), &session_list);

	if (mdrcnt > 0) {
		*rdata = smb_realloc_limit(*rdata,mdrcnt);
		if (!*rdata) {
			return False;
		}
	}
	memset((char *)&desc,'\0',sizeof(desc));
	desc.base = *rdata;
	desc.buflen = mdrcnt;
	desc.format = str2;
	if (!init_package(&desc,num_sessions,0)) {
		return False;
	}

	for(i=0; i<num_sessions; i++) {
		PACKS(&desc, "z", session_list[i].remote_machine);
		PACKS(&desc, "z", session_list[i].username);
		PACKI(&desc, "W", 1); /* num conns */
		PACKI(&desc, "W", 0); /* num opens */
		PACKI(&desc, "W", 1); /* num users */
		PACKI(&desc, "D", 0); /* session time */
		PACKI(&desc, "D", 0); /* idle time */
		PACKI(&desc, "D", 0); /* flags */
		PACKS(&desc, "z", "Unknown Client"); /* client type string */
	}

	*rdata_len = desc.usedlen;

	*rparam_len = 8;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}
	SSVALS(*rparam,0,desc.errcode);
	SSVAL(*rparam,2,0); /* converter */
	SSVAL(*rparam,4,num_sessions); /* count */

	DEBUG(4,("RNetSessionEnum: errorcode %d\n",desc.errcode));

	return True;
}


/****************************************************************************
 The buffer was too small.
 ****************************************************************************/

static bool api_TooSmall(connection_struct *conn,uint16 vuid, char *param, char *data,
			 int mdrcnt, int mprcnt,
			 char **rdata, char **rparam,
			 int *rdata_len, int *rparam_len)
{
	*rparam_len = MIN(*rparam_len,mprcnt);
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	*rdata_len = 0;

	SSVAL(*rparam,0,NERR_BufTooSmall);

	DEBUG(3,("Supplied buffer too small in API command\n"));

	return True;
}

/****************************************************************************
 The request is not supported.
 ****************************************************************************/

static bool api_Unsupported(connection_struct *conn, uint16 vuid,
				char *param, int tpscnt,
				char *data, int tdscnt,
				int mdrcnt, int mprcnt,
				char **rdata, char **rparam,
				int *rdata_len, int *rparam_len)
{
	*rparam_len = 4;
	*rparam = smb_realloc_limit(*rparam,*rparam_len);
	if (!*rparam) {
		return False;
	}

	*rdata_len = 0;

	SSVAL(*rparam,0,NERR_notsupported);
	SSVAL(*rparam,2,0);		/* converter word */

	DEBUG(3,("Unsupported API command\n"));

	return True;
}

static const struct {
	const char *name;
	int id;
	bool (*fn)(connection_struct *, uint16,
			char *, int,
			char *, int,
			int,int,char **,char **,int *,int *);
	bool auth_user;		/* Deny anonymous access? */
} api_commands[] = {
	{"RNetShareEnum",	RAP_WshareEnum,		api_RNetShareEnum, True},
	{"RNetShareGetInfo",	RAP_WshareGetInfo,	api_RNetShareGetInfo},
	{"RNetShareAdd",	RAP_WshareAdd,		api_RNetShareAdd},
	{"RNetSessionEnum",	RAP_WsessionEnum,	api_RNetSessionEnum, True},
	{"RNetServerGetInfo",	RAP_WserverGetInfo,	api_RNetServerGetInfo},
	{"RNetGroupEnum",	RAP_WGroupEnum,		api_RNetGroupEnum, True},
	{"RNetGroupGetUsers", RAP_WGroupGetUsers,	api_RNetGroupGetUsers, True},
	{"RNetUserEnum", 	RAP_WUserEnum,		api_RNetUserEnum, True},
	{"RNetUserGetInfo",	RAP_WUserGetInfo,	api_RNetUserGetInfo},
	{"NetUserGetGroups",	RAP_WUserGetGroups,	api_NetUserGetGroups},
	{"NetWkstaGetInfo",	RAP_WWkstaGetInfo,	api_NetWkstaGetInfo},
	{"DosPrintQEnum",	RAP_WPrintQEnum,	api_DosPrintQEnum, True},
	{"DosPrintQGetInfo",	RAP_WPrintQGetInfo,	api_DosPrintQGetInfo},
	{"WPrintQueuePause",  RAP_WPrintQPause,	api_WPrintQueueCtrl},
	{"WPrintQueueResume", RAP_WPrintQContinue,	api_WPrintQueueCtrl},
	{"WPrintJobEnumerate",RAP_WPrintJobEnum,	api_WPrintJobEnumerate},
	{"WPrintJobGetInfo",	RAP_WPrintJobGetInfo,	api_WPrintJobGetInfo},
	{"RDosPrintJobDel",	RAP_WPrintJobDel,	api_RDosPrintJobDel},
	{"RDosPrintJobPause",	RAP_WPrintJobPause,	api_RDosPrintJobDel},
	{"RDosPrintJobResume",RAP_WPrintJobContinue,	api_RDosPrintJobDel},
	{"WPrintDestEnum",	RAP_WPrintDestEnum,	api_WPrintDestEnum},
	{"WPrintDestGetInfo",	RAP_WPrintDestGetInfo,	api_WPrintDestGetInfo},
	{"NetRemoteTOD",	RAP_NetRemoteTOD,	api_NetRemoteTOD},
	{"WPrintQueuePurge",	RAP_WPrintQPurge,	api_WPrintQueueCtrl},
	{"NetServerEnum",	RAP_NetServerEnum2,	api_RNetServerEnum}, /* anon OK */
	{"WAccessGetUserPerms",RAP_WAccessGetUserPerms,api_WAccessGetUserPerms},
	{"SetUserPassword",	RAP_WUserPasswordSet2,	api_SetUserPassword},
	{"WWkstaUserLogon",	RAP_WWkstaUserLogon,	api_WWkstaUserLogon},
	{"PrintJobInfo",	RAP_WPrintJobSetInfo,	api_PrintJobInfo},
	{"WPrintDriverEnum",	RAP_WPrintDriverEnum,	api_WPrintDriverEnum},
	{"WPrintQProcEnum",	RAP_WPrintQProcessorEnum,api_WPrintQProcEnum},
	{"WPrintPortEnum",	RAP_WPrintPortEnum,	api_WPrintPortEnum},
	{"SamOEMChangePassword",RAP_SamOEMChgPasswordUser2_P,api_SamOEMChangePassword}, /* anon OK */
	{NULL,		-1,	api_Unsupported}
	/*  The following RAP calls are not implemented by Samba:

	RAP_WFileEnum2 - anon not OK 
	*/
};


/****************************************************************************
 Handle remote api calls.
****************************************************************************/

void api_reply(connection_struct *conn, uint16 vuid,
	       struct smb_request *req,
	       char *data, char *params,
	       int tdscnt, int tpscnt,
	       int mdrcnt, int mprcnt)
{
	int api_command;
	char *rdata = NULL;
	char *rparam = NULL;
	const char *name1 = NULL;
	const char *name2 = NULL;
	int rdata_len = 0;
	int rparam_len = 0;
	bool reply=False;
	int i;

	if (!params) {
		DEBUG(0,("ERROR: NULL params in api_reply()\n"));
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return;
	}

	if (tpscnt < 2) {
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return;
	}
	api_command = SVAL(params,0);
	/* Is there a string at position params+2 ? */
	if (skip_string(params,tpscnt,params+2)) {
		name1 = params + 2;
	} else {
		name1 = "";
	}
	name2 = skip_string(params,tpscnt,params+2);
	if (!name2) {
		name2 = "";
	}

	DEBUG(3,("Got API command %d of form <%s> <%s> (tdscnt=%d,tpscnt=%d,mdrcnt=%d,mprcnt=%d)\n",
		api_command,
		name1,
		name2,
		tdscnt,tpscnt,mdrcnt,mprcnt));

	for (i=0;api_commands[i].name;i++) {
		if (api_commands[i].id == api_command && api_commands[i].fn) {
			DEBUG(3,("Doing %s\n",api_commands[i].name));
			break;
		}
	}

	/* Check whether this api call can be done anonymously */

	if (api_commands[i].auth_user && lp_restrict_anonymous()) {
		user_struct *user = get_valid_user_struct(vuid);

		if (!user || user->guest) {
			reply_nterror(req, NT_STATUS_ACCESS_DENIED);
			return;
		}
	}

	rdata = (char *)SMB_MALLOC(1024);
	if (rdata) {
		memset(rdata,'\0',1024);
	}

	rparam = (char *)SMB_MALLOC(1024);
	if (rparam) {
		memset(rparam,'\0',1024);
	}

	if(!rdata || !rparam) {
		DEBUG(0,("api_reply: malloc fail !\n"));
		SAFE_FREE(rdata);
		SAFE_FREE(rparam);
		reply_nterror(req, NT_STATUS_NO_MEMORY);
		return;
	}

	reply = api_commands[i].fn(conn,
				vuid,
				params,tpscnt,	/* params + length */
				data,tdscnt,	/* data + length */
				mdrcnt,mprcnt,
				&rdata,&rparam,&rdata_len,&rparam_len);


	if (rdata_len > mdrcnt || rparam_len > mprcnt) {
		reply = api_TooSmall(conn,vuid,params,data,mdrcnt,mprcnt,
					&rdata,&rparam,&rdata_len,&rparam_len);
	}

	/* if we get False back then it's actually unsupported */
	if (!reply) {
		reply = api_Unsupported(conn,vuid,params,tpscnt,data,tdscnt,mdrcnt,mprcnt,
			&rdata,&rparam,&rdata_len,&rparam_len);
	}

	/* If api_Unsupported returns false we can't return anything. */
	if (reply) {
		send_trans_reply(conn, req, rparam, rparam_len,
				 rdata, rdata_len, False);
	}

	SAFE_FREE(rdata);
	SAFE_FREE(rparam);
	return;
}
