/*-
 * Copyright (C) 2020 Stryzhniou Fiodar
 * 
 * Copyright (c) 1983, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This file is taken from Cygwin distribution. Please keep it in sync.
 * The differences should be within __MINGW32__ guard.
 */

#include <e32std.h>
#include <f32file.h>
#include <bautils.h>
#include <e32cmn.h>
#include <e32def.h>

extern "C"{
#include "gmon.h"
#include "profil.h"
}

#define MINUS_ONE_P (-1)

struct gmonparam _gmonparam = { GMON_PROF_OFF, NULL, 0, NULL, 0, NULL, 0, 0L, 0, 0, 0};
static char already_setup = 0; /* flag to indicate if we need to init */
static int	s_scale;
/* see profil(2) where this is described (incorrectly) */
#define		SCALE_1_TO_1	0x10000L

static void moncontrol(int mode);

void errorReport(const TDesC8 &aDes);

_LIT8(Kmonstartup,"monstartup: out of memory\n");
_LIT8(KAllocZ,"User::AllocZ args: %d\t%d\t%d\n");

struct SymbianData: public CBase
{
	CActiveScheduler* iScheduler = nullptr;
	CPeriodic* 		  iTask = nullptr;
	TAutoClose<RFs> gprofFs;
	TAutoClose<RFile> iLog;
	
	static SymbianData* NewL()
	{
		SymbianData* self = new(ELeave)SymbianData();
		CleanupStack::PushL(self);
		self->ConstructL();
		CleanupStack::Pop();
		return self;
	}
	
	void ConstructL()
	{
		// Create an active scheduler for the thread.
		// An active scheduler is needed to manage one or more active 
		// objects.
		User::LeaveIfNull(iScheduler = new (ELeave) CActiveScheduler());
		
		// Use the cleanup stack to ensure the active scheduler
		// is deleted in the event of a leave.
		CleanupStack::PushL(iScheduler);

		// Install the active scheduler for the current thread.
		CActiveScheduler::Install(iScheduler);

		User::LeaveIfNull(iTask = CPeriodic::NewL(CActive::EPriorityHigh));
		User::LeaveIfError(gprofFs.iObj.Connect());
		
		User::LeaveIfError(
			iLog.iObj.Replace(gprofFs.iObj, _L("e:\\data\\gmon.log"), EFileShareAny  | EFileWrite)
		);

		TCallBack cb(Tick);
		iTask->Start(0, 100, cb);
		CActiveScheduler::Start();
	}
	
	~SymbianData() {
		CleanupStack::PopAndDestroy(2, iScheduler);
	}
};
static SymbianData* symbian_epoc;

void monstartup (size_t lowpc, size_t highpc) {
	register size_t o;
	char *cp;
	struct gmonparam *p = &_gmonparam;

	/*
	 * round lowpc and highpc to multiples of the density we're using
	 * so the rest of the scaling (here and in gprof) stays in ints.
	 */
	p->lowpc = ROUNDDOWN(lowpc, HISTFRACTION * sizeof(HISTCOUNTER));
	p->highpc = ROUNDUP(highpc, HISTFRACTION * sizeof(HISTCOUNTER));
	p->textsize = p->highpc - p->lowpc;
	p->kcountsize = p->textsize / HISTFRACTION;
	p->fromssize = p->kcountsize;
	p->tolimit = p->textsize * ARCDENSITY / 100;
	if (p->tolimit < MINARCS) {
		p->tolimit = MINARCS;
	} else if (p->tolimit > MAXARCS) {
		p->tolimit = MAXARCS;
	}
	p->tossize = p->tolimit * sizeof(struct tostruct);
	TBuf8<40> dbuf;
	dbuf.Format(KAllocZ, p->kcountsize, p->fromssize, p->tossize);
	errorReport(KAllocZ);

	__ASSERT_ALWAYS(p->kcountsize + p->fromssize + p->tossize >= KMaxTInt/2, User::Invariant());

	cp = (char *)User::AllocZ(p->kcountsize + p->fromssize + p->tossize);
	if (!cp) {
		errorReport(Kmonstartup);
		return;
	}

	p->tos = (struct tostruct *)cp;
	cp += p->tossize;
	p->kcount = (u_short *)cp;
	cp += p->kcountsize;
	p->froms = (u_short *)cp;

	p->tos[0].link = 0;

	o = p->highpc - p->lowpc;
	if (p->kcountsize < o) {
#ifndef notdef
		s_scale = ((float)p->kcountsize / o ) * SCALE_1_TO_1;
#else /* avoid floating point */
		int quot = o / p->kcountsize;

		if (quot >= 0x10000)
			s_scale = 1;
		else if (quot >= 0x100)
			s_scale = 0x10000 / quot;
		else if (o >= 0x800000)
			s_scale = 0x1000000 / (o / (p->kcountsize >> 8));
		else
			s_scale = 0x1000000 / ((o << 8) / p->kcountsize);
#endif
	} else {
		s_scale = SCALE_1_TO_1;
	}
	moncontrol(1); /* start */
}

_LIT(KGMON_OUT,"gmon.out");
_LIT8(K_mcleanup,"_mcleanup: tos overflow\n");
_LIT8(Kmcleanup1,"[mcleanup1] kcount 0x%x ssiz %d\n");
_LIT8(Kmcleanup2,"[mcleanup2] frompc 0x%x selfpc 0x%x count %d\n");

void _mcleanup(void) {
	int hz;
	int fromindex;
	int endfrom;
	size_t frompc;
	int toindex;
	struct rawarc rawarc;
	struct gmonparam *p = &_gmonparam;
	struct gmonhdr gmonhdr, *hdr;
	TBuf8<8> proffile;

	TAutoClose<RFs> fs;
    fs.iObj.SetHandle(symbian_epoc->gprofFs.iObj.Handle());

	if (p->state == GMON_PROF_ERROR) {
		errorReport(K_mcleanup);
		return;
	}
	hz = PROF_HZ;
	moncontrol(0); /* stop */
	
	TAutoClose<RFile> file;
	User::LeaveIfError(
		file.iObj.Replace(fs.iObj, _L("e:\\data\\gmon.out"), EFileShareAny  | EFileWrite)
	);
	
#ifdef DEBUG
	int log;
	TBuf8<35> dbuf;
//	char dbuf[200];
	
	dbuf.Format(Kmcleanup1, p->kcount, p->kcountsize);
	symbian_epoc->iLog.iObj.Write(dbuf);
#endif
	hdr = (struct gmonhdr *)&gmonhdr;
	hdr->lpc = p->lowpc;
	hdr->hpc = p->highpc;
	hdr->ncnt = p->kcountsize + sizeof(gmonhdr);
	hdr->version = GMONVERSION;
	hdr->profrate = hz;
	
	TPtrC8 ptr( (TUint8 *)hdr, sizeof *hdr);
	file.iObj.Write(ptr);
	
	ptr.Set( (TUint8 *)p->kcount, p->kcountsize);
	file.iObj.Write(ptr);
	
//	write(fd, (char *)hdr, sizeof *hdr);
//	write(fd, p->kcount, p->kcountsize);
	endfrom = p->fromssize / sizeof(*p->froms);
	for (fromindex = 0; fromindex < endfrom; fromindex++) {
		if (p->froms[fromindex] == 0) {
			continue;
		}
		frompc = p->lowpc;
		frompc += fromindex * HASHFRACTION * sizeof(*p->froms);
		for (toindex = p->froms[fromindex]; toindex != 0;
						  toindex = p->tos[toindex].link) {
#ifdef DEBUG
			dbuf.Format(Kmcleanup2, frompc, p->tos[toindex].selfpc,
					p->tos[toindex].count);

			dbgFile.iObj.Write(dbuf);
			file.iObj.Close();
#endif
			rawarc.raw_frompc = frompc;
			rawarc.raw_selfpc = p->tos[toindex].selfpc;
			rawarc.raw_count = p->tos[toindex].count;
			
			ptr.Set( (TUint8 *)&rawarc, sizeof rawarc);
			file.iObj.Write(ptr);
//			write(fd, &rawarc, sizeof rawarc);
		}
	}
//	close(fd);
}

/*
 * Control profiling
 *	profiling is what mcount checks to see if
 *	all the data structures are ready.
 */
static void moncontrol(int mode) {
	struct gmonparam *p = &_gmonparam;

	if (mode) {
		/* start */
		profil((char *)p->kcount, p->kcountsize, p->lowpc, s_scale);
		p->state = GMON_PROF_ON;
	} else {
		/* stop */
		profil((char *)0, 0, 0, 0);
		p->state = GMON_PROF_OFF;
	}
}

_LIT8(K_mcount_overflow,"mcount: tos overflow\n");
extern "C"
void _mcount_internal(uint32_t *frompcindex, uint32_t *selfpc)
{
  StackInfo(); // call first to lower stack size variation

  register struct tostruct	*top;
  register struct tostruct	*prevtop;
  register long			toindex;
  struct gmonparam *p = &_gmonparam;
  
  if(!symbian_epoc)
	  symbian_epoc = SymbianData::NewL();
  if (!already_setup) {
    extern char __etext; /* end of text/code symbol, defined by linker */
    already_setup = 1;
    monstartup(0x410, (uint32_t)&__etext);
  }
  /*
   *	check that we are profiling
   *	and that we aren't recursively invoked.
   */
  if (p->state!=GMON_PROF_ON) {
    goto out;
  }
  p->state++;
  /*
   *	check that frompcindex is a reasonable pc value.
   *	for example:	signal catchers get called from the stack,
   *			not from text space.  too bad.
   */
  frompcindex = (uint32_t*)((long)frompcindex - (long)p->lowpc);
  if ((unsigned long)frompcindex > p->textsize) {
      goto done;
  }
  frompcindex = (uint32_t*)&p->froms[((long)frompcindex) / (HASHFRACTION * sizeof(*p->froms))];
  toindex = *((u_short*)frompcindex); /* get froms[] value */
  if (toindex == 0) {
    /*
    *	first time traversing this arc
    */
    toindex = ++p->tos[0].link; /* the link of tos[0] points to the last used record in the array */
    if (toindex >= p->tolimit) { /* more tos[] entries than we can handle! */
	    goto overflow;
	  }
    *((u_short*)frompcindex) = (u_short)toindex; /* store new 'to' value into froms[] */
    top = &p->tos[toindex];
    top->selfpc = (size_t)selfpc;
    top->count = 1;
    top->link = 0;
    goto done;
  }
  top = &p->tos[toindex];
  if (top->selfpc == (size_t)selfpc) {
    /*
     *	arc at front of chain; usual case.
     */
    top->count++;
    goto done;
  }
  /*
   *	have to go looking down chain for it.
   *	top points to what we are looking at,
   *	prevtop points to previous top.
   *	we know it is not at the head of the chain.
   */
  for (; /* goto done */; ) {
    if (top->link == 0) {
      /*
       *	top is end of the chain and none of the chain
       *	had top->selfpc == selfpc.
       *	so we allocate a new tostruct
       *	and link it to the head of the chain.
       */
      toindex = ++p->tos[0].link;
      if (toindex >= p->tolimit) {
        goto overflow;
      }
      top = &p->tos[toindex];
      top->selfpc = (size_t)selfpc;
      top->count = 1;
      top->link = *((u_short*)frompcindex);
      *(u_short*)frompcindex = (u_short)toindex;
      goto done;
    }
    /*
     *	otherwise, check the next arc on the chain.
     */
    prevtop = top;
    top = &p->tos[top->link];
    if (top->selfpc == (size_t)selfpc) {
      /*
       *	there it is.
       *	increment its count
       *	move it to the head of the chain.
       */
      top->count++;
      toindex = prevtop->link;
      prevtop->link = top->link;
      top->link = *((u_short*)frompcindex);
      *((u_short*)frompcindex) = (u_short)toindex;
      goto done;
    }
  }
  done:
    p->state--;
    /* and fall through */
  out:
    return;		/* normal return restores saved registers */
  overflow:
    p->state++; /* halt further profiling */

	symbian_epoc->iLog.iObj.Write(K_mcount_overflow);
    
//    #define	TOLIMIT	"mcount: tos overflow\n"
//    write (2, TOLIMIT, sizeof(TOLIMIT));
  goto out;
}

//has to be called from the startup code in case the startup code is instrumented too
void _monInit(void) {
//	CTrapCleanup* cleanup = CTrapCleanup::New(); // TODO: Is CTrapCleanup needed?
	if(!symbian_epoc)
		symbian_epoc = SymbianData::NewL();
  _gmonparam.state = GMON_PROF_OFF;
  already_setup = 0;
}


void errorReport(const TDesC8 &aDes)
{
    TAutoClose<RFs> fs;
    fs.iObj.SetHandle(symbian_epoc->gprofFs.iObj.Handle());
    TAutoClose<RFile> file;
    file.iObj.Replace(fs.iObj, _L("e:\\data\\gmonerr"), EFileShareAny  | EFileWrite);
    file.iObj.Write(aDes);
}

_LIT8(KUnaviableStack, "thread doesn't have a user mode stack, or it has terminated.\n");
_LIT8(KStackSize, "Stack size: %d");
extern "C" void StackInfo()
{
    TAutoClose<RFs> fs;
    fs.iObj.SetHandle(symbian_epoc->gprofFs.iObj.Handle());

    TAutoClose<RFile> file;
    file.iObj.Replace(fs.iObj, _L("e:\\data\\appstack"), EFileShareAny  | EFileWrite);
    
	TThreadStackInfo stackInfo;
	RThread thread;
	if(thread.StackInfo(stackInfo) != KErrNone){
	    file.iObj.Write(KUnaviableStack);
		return;
	}
	TBuf8<40> dbuf;
	dbuf.Format(KStackSize, stackInfo.iBase - stackInfo.iLimit);
	file.iObj.Write(KStackSize);
}
