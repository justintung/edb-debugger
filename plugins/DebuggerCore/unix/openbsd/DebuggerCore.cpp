/*
Copyright (C) 2006 - 2011 Evan Teran
                          eteran@alum.rit.edu

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "DebuggerCore.h"
#include "State.h"
#include "DebugEvent.h"
#include "PlatformState.h"
#include "PlatformRegion.h"

#include <boost/bind.hpp>

#include <QDebug>
#include <QMessageBox>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <kvm.h>
#include <machine/reg.h>
#include <paths.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define __need_process
#include <sys/sysctl.h>
#include <sys/proc.h>

namespace {
inline int resume_code(int status) {
	if(WIFSIGNALED(status)) {
		return WTERMSIG(status);
	} else if(WIFSTOPPED(status)) {
		return WSTOPSIG(status);
	}
	return 0;
}
}

//------------------------------------------------------------------------------
// Name: DebuggerCore()
// Desc: constructor
//------------------------------------------------------------------------------
DebuggerCore::DebuggerCore() {
#if defined(_SC_PAGESIZE)
	page_size_ = sysconf(_SC_PAGESIZE);
#elif defined(_SC_PAGE_SIZE)
	page_size_ = sysconf(_SC_PAGE_SIZE);
#else
	page_size_ = PAGE_SIZE;
#endif
}

//------------------------------------------------------------------------------
// Name: 
// Desc: 
//------------------------------------------------------------------------------
bool DebuggerCore::has_extension(quint64 ext) const {
	return false;
}

//------------------------------------------------------------------------------
// Name: page_size() const
// Desc: returns the size of a page on this system
//------------------------------------------------------------------------------
edb::address_t DebuggerCore::page_size() const {
	return page_size_;
}

//------------------------------------------------------------------------------
// Name: ~DebuggerCore()
// Desc:
//------------------------------------------------------------------------------
DebuggerCore::~DebuggerCore() {
	detach();
}

//------------------------------------------------------------------------------
// Name: wait_debug_event(DebugEvent &event, int msecs)
// Desc: waits for a debug event, msecs is a timeout
//      it will return false if an error or timeout occurs
//------------------------------------------------------------------------------
bool DebuggerCore::wait_debug_event(DebugEvent &event, int msecs) {
	if(attached()) {
		int status;
		bool timeout;

		const edb::tid_t tid = native::waitpid_timeout(pid(), &status, 0, msecs, timeout);
		if(!timeout) {
			if(tid > 0) {

				event = DebugEvent(status, pid(), tid);
				active_thread_       = event.thread();
				threads_[tid].status = status;
				return true;
			}
		}
	}
	return false;
}

//------------------------------------------------------------------------------
// Name: read_data(edb::address_t address, bool &ok)
// Desc:
//------------------------------------------------------------------------------
long DebuggerCore::read_data(edb::address_t address, bool &ok) {
	// NOTE: this will fail on newer versions of linux if called from a
	// different thread than the one which attached to process
	errno = 0;
	const long v = ptrace(PT_READ_D, pid(), reinterpret_cast<char*>(address), 0);
	SET_OK(ok, v);
	return v;
}

//------------------------------------------------------------------------------
// Name: write_data(edb::address_t address, long value)
// Desc:
//------------------------------------------------------------------------------
bool DebuggerCore::write_data(edb::address_t address, long value) {
	return ptrace(PT_WRITE_D, pid(), reinterpret_cast<char*>(address), value) != -1;
}

//------------------------------------------------------------------------------
// Name: attach(edb::pid_t pid)
// Desc:
//------------------------------------------------------------------------------
bool DebuggerCore::attach(edb::pid_t pid) {
	detach();

	const long ret = ptrace(PT_ATTACH, pid, 0, 0);
	if(ret == 0) {
		pid_           = pid;
		active_thread_ = pid;
		threads_.clear();
		threads_.insert(pid, thread_info());

		// TODO: attach to all of the threads
	}

	return ret == 0;
}

//------------------------------------------------------------------------------
// Name: detach()
// Desc:
//------------------------------------------------------------------------------
void DebuggerCore::detach() {
	if(attached()) {
	
		// TODO: do i need to stop each thread first, and wait for them?
	
		clear_breakpoints();
		ptrace(PT_DETACH, pid(), 0, 0);
		pid_ = 0;
		threads_.clear();
	}
}

//------------------------------------------------------------------------------
// Name: kill()
// Desc:
//------------------------------------------------------------------------------
void DebuggerCore::kill() {
	if(attached()) {
		clear_breakpoints();
		ptrace(PT_KILL, pid(), 0, 0);
		native::waitpid(pid(), 0, WAIT_ANY);
		pid_ = 0;
		threads_.clear();
	}
}

//------------------------------------------------------------------------------
// Name: pause()
// Desc: stops *all* threads of a process
//------------------------------------------------------------------------------
void DebuggerCore::pause() {
	if(attached()) {
		for(threadmap_t::const_iterator it = threads_.begin(); it != threads_.end(); ++it) {
			::kill(it.key(), SIGSTOP);
		}
	}
}

//------------------------------------------------------------------------------
// Name: resume(edb::EVENT_STATUS status)
// Desc:
//------------------------------------------------------------------------------
void DebuggerCore::resume(edb::EVENT_STATUS status) {
	// TODO: assert that we are paused

	if(attached()) {
		if(status != edb::DEBUG_STOP) {
			const edb::tid_t tid = active_thread();
			const int code = (status == edb::DEBUG_EXCEPTION_NOT_HANDLED) ? resume_code(threads_[tid].status) : 0;
			ptrace(PT_CONTINUE, tid, reinterpret_cast<caddr_t>(1), code);
		}
	}
}

//------------------------------------------------------------------------------
// Name: step(edb::EVENT_STATUS status)
// Desc:
//------------------------------------------------------------------------------
void DebuggerCore::step(edb::EVENT_STATUS status) {
	// TODO: assert that we are paused

	if(attached()) {
		if(status != edb::DEBUG_STOP) {
			const edb::tid_t tid = active_thread();
			const int code = (status == edb::DEBUG_EXCEPTION_NOT_HANDLED) ? resume_code(threads_[tid].status) : 0;
			ptrace(PT_STEP, tid, reinterpret_cast<caddr_t>(1), code);
		}
	}
}

//------------------------------------------------------------------------------
// Name: get_state(State &state)
// Desc:
//------------------------------------------------------------------------------
void DebuggerCore::get_state(State &state) {
	// TODO: assert that we are paused
	PlatformState *const state_impl = static_cast<PlatformState *>(state.impl_);

	if(attached()) {
		if(ptrace(PT_GETREGS, active_thread(), reinterpret_cast<char*>(&state_impl->regs_), 0) != -1) {
			// TODO
			state_impl->gs_base = 0;
			state_impl->fs_base = 0;
		}

		if(ptrace(PT_GETFPREGS, active_thread(), reinterpret_cast<char*>(&state_impl->fpregs_), 0) != -1) {
		}

		// TODO: Debug Registers

	} else {
		state.clear();
	}
}

//------------------------------------------------------------------------------
// Name: set_state(const State &state)
// Desc:
//------------------------------------------------------------------------------
void DebuggerCore::set_state(const State &state) {

	// TODO: assert that we are paused
	PlatformState *const state_impl = static_cast<PlatformState *>(state.impl_);

	if(attached()) {
		ptrace(PT_SETREGS, active_thread(), reinterpret_cast<char*>(&state_impl->regs_), 0);

		// TODO: FPU
		// TODO: Debug Registers
	}
}

//------------------------------------------------------------------------------
// Name: open(const QString &path, const QString &cwd, const QStringList &args, const QString &tty)
// Desc:
//------------------------------------------------------------------------------
bool DebuggerCore::open(const QString &path, const QString &cwd, const QStringList &args, const QString &tty) {
	detach();
	pid_t pid;

	switch(pid = fork()) {
	case 0:
		// we are in the child now...

		// set ourselves (the child proc) up to be traced
		ptrace(PT_TRACE_ME, 0, 0, 0);

		// redirect it's I/O
		if(!tty.isEmpty()) {
			FILE *const std_out = freopen(qPrintable(tty), "r+b", stdout);
			FILE *const std_in  = freopen(qPrintable(tty), "r+b", stdin);
			FILE *const std_err = freopen(qPrintable(tty), "r+b", stderr);

			Q_UNUSED(std_out);
			Q_UNUSED(std_in);
			Q_UNUSED(std_err);
		}

		// do the actual exec
		execute_process(path, cwd, args);

		// we should never get here!
		abort();
		break;
	case -1:
		// error!
		pid_ = 0;
		return false;
	default:
		// parent
		do {
			threads_.clear();

			int status;
			if(native::waitpid(pid, &status, 0) == -1) {
				return false;
			}

			// the very first event should be a STOP of type SIGTRAP
			if(!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
				detach();
				return false;
			}

			// setup the first event data for the primary thread
			threads_.insert(pid, thread_info());
			pid_                 = pid;
			active_thread_       = pid;
			threads_[pid].status = status;
			return true;
		} while(0);
		break;
	}
}

//------------------------------------------------------------------------------
// Name: set_active_thread(edb::tid_t tid)
// Desc:
//------------------------------------------------------------------------------
void DebuggerCore::set_active_thread(edb::tid_t tid) {
	Q_ASSERT(threads_.contains(tid));
	active_thread_ = tid;
}

//------------------------------------------------------------------------------
// Name: create_state() const
// Desc:
//------------------------------------------------------------------------------
IState *DebuggerCore::create_state() const {
	return new PlatformState;
}

//------------------------------------------------------------------------------
// Name: create_region(edb::address_t start, edb::address_t end, edb::address_t base, const QString &name, IRegion::permissions_t permissions)
// Desc:
//------------------------------------------------------------------------------
IRegion *DebuggerCore::create_region(edb::address_t start, edb::address_t end, edb::address_t base, const QString &name, IRegion::permissions_t permissions) const {
	return new PlatformRegion(start, end, base, name, permissions);
}

//------------------------------------------------------------------------------
// Name: enumerate_processes() const
// Desc:
//------------------------------------------------------------------------------
QMap<edb::pid_t, Process> DebuggerCore::enumerate_processes() const {
	QMap<edb::pid_t, Process> ret;
	
	char ebuffer[_POSIX2_LINE_MAX];
	int numprocs;

	kvm_t *const kaccess = kvm_openfiles(NULL, NULL, NULL, O_RDONLY, ebuffer);
	if(kaccess != 0) {
		struct kinfo_proc *const kprocaccess = kvm_getprocs(kaccess, KERN_PROC_ALL, 0, sizeof *kprocaccess, &numprocs);
		if(kprocaccess != 0) {
			for(int i = 0; i < numprocs; ++i) {
				Process procInfo;
				procInfo.pid  = kprocaccess[i].p_pid;
				procInfo.uid  = kprocaccess[i].p_uid;
				procInfo.name = kprocaccess[i].p_comm;

				ret.insert(procInfo.pid, procInfo);
			}
		}
		kvm_close(kaccess);
	} else {
		QMessageBox::warning(0, "Error Listing Processes", ebuffer);
	}
	
	return ret;
}

//------------------------------------------------------------------------------
// Name: 
// Desc:
//------------------------------------------------------------------------------
QString DebuggerCore::process_exe(edb::pid_t pid) const {
	QString ret;

	char errbuf[_POSIX2_LINE_MAX];
	if(kvm_t *kd = kvm_openfiles(NULL, NULL, NULL, O_RDONLY, errbuf)) {

		int rc;
		if(struct kinfo_proc *const proc = kvm_getprocs(kd, KERN_PROC_PID, pid, sizeof(struct kinfo_proc), &rc)) {
			char p_comm[KI_MAXCOMLEN] = "";
			memcpy(p_comm, proc->p_comm, sizeof(p_comm));
		}

		kvm_close(kd);
		return p_comm;
	}

	return ret;
}

//------------------------------------------------------------------------------
// Name: 
// Desc:
//------------------------------------------------------------------------------
QString DebuggerCore::process_cwd(edb::pid_t pid) const {
	// TODO: implement this
	return QString();
}

//------------------------------------------------------------------------------
// Name: 
// Desc:
//------------------------------------------------------------------------------
edb::pid_t DebuggerCore::parent_pid(edb::pid_t pid) const {
	edb::pid_t ret = 0;
	char errbuf[_POSIX2_LINE_MAX];
	if(kvm_t *kd = kvm_openfiles(NULL, NULL, NULL, O_RDONLY, errbuf)) {
		int rc;
		struct kinfo_proc *const proc = kvm_getprocs(kd, KERN_PROC_PID, pid, sizeof *proc, &rc);
		ret = proc->p_ppid;
		kvm_close(kd);
	}
	return ret;
}

Q_EXPORT_PLUGIN2(DebuggerCore, DebuggerCore)
