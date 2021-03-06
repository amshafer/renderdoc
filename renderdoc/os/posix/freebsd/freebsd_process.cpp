/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2020 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <elf.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#include <libutil.h>
#include <machine/reg.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include "api/replay/data_types.h"
#include "common/common.h"
#include "common/formatting.h"
#include "core/core.h"
#include "core/settings.h"
#include "os/os_specific.h"

RDOC_CONFIG(bool, FreeBSD_PtraceChildProcesses, true,
            "Use ptrace(2) to trace child processes at startup to ensure connection is made as "
            "early as possible.");
RDOC_CONFIG(bool, FreeBSD_Debug_PtraceLogging, true,
            "Enable verbose debug logging of ptrace usage.");

extern char **environ;

// we wait 1ns, then 2ns, then 4ns, etc so our total is 0xfff etc
// 0xfffff == ~1s
#define INITIAL_WAIT_TIME 1
#define MAX_WAIT_TIME 0xfffff

char **GetCurrentEnvironment()
{
  return environ;
}

rdcarray<int> getSockets(pid_t childPid)
{
  rdcarray<int> sockets;
  rdcstr dirPath = StringFormat::Fmt("/proc/%d/fd", (int)childPid);
  rdcarray<PathEntry> files;
  FileIO::GetFilesInDirectory(dirPath, files);
  if(files.empty())
    return sockets;

  for(const PathEntry &file : files)
  {
    rdcstr target = StringFormat::Fmt("%s/%s", dirPath.c_str(), file.filename.c_str());
    char linkname[1024];
    ssize_t length = readlink(target.c_str(), linkname, 1023);
    if(length == -1)
      continue;

    linkname[length] = '\0';
    uint32_t inode = 0;
    int num = sscanf(linkname, "socket:[%u]", &inode);
    if(num == 1)
      sockets.push_back(inode);
  }
  return sockets;
}

rdcstr execcmd(const char *cmd)
{
  FILE *pipe = popen(cmd, "r");

  if(!pipe)
    return "ERROR";

  char buffer[128];

  rdcstr result = "";

  while(!feof(pipe))
  {
    if(fgets(buffer, 128, pipe) != NULL)
      result += buffer;
  }

  pclose(pipe);

  return result;
}

bool isNewline(char c)
{
  return c == '\n' || c == '\r';
}

int GetIdentPort(pid_t childPid)
{
  rdcstr lsof = StringFormat::Fmt("lsof -p %d -a -i 4 -F n", (int)childPid);
  rdcstr result;
  uint32_t wait = 1;
  for(int i = 0; i < 10; ++i)
  {
    result = execcmd(lsof.c_str());
    if(!result.empty())
      break;
    usleep(wait * 1000);
    wait *= 2;
  }
  if(result.empty())
  {
    RDCERR("No output from lsof command: '%s'", lsof.c_str());
    return 0;
  }

  // Parse the result expecting:
  // p<PID>
  // <TEXT>
  // n*:<PORT>

  rdcstr parseResult(result);
  const size_t len = parseResult.length();
  if(parseResult[0] == 'p')
  {
    size_t tokenStart = 1;
    size_t i = tokenStart;
    for(; i < len; i++)
    {
      if(parseResult[i] < '0' || parseResult[i] > '9')
        break;
    }
    parseResult[i++] = 0;

    if(isNewline(parseResult[i]))
      i++;

    const int pid = atoi(&result[tokenStart]);
    if(pid == (int)childPid)
    {
      const char *netString("n*:");
      while(i < len)
      {
        const int netStart = parseResult.find(netString, i);
        if(netStart >= 0)
        {
          tokenStart = netStart + strlen(netString);
          i = tokenStart;
          for(; i < len; i++)
          {
            if(parseResult[i] < '0' || parseResult[i] > '9')
              break;
          }
          parseResult[i++] = 0;

          if(isNewline(parseResult[i]))
            i++;

          const int port = atoi(&result[tokenStart]);
          if(port >= RenderDoc_FirstTargetControlPort && port <= RenderDoc_LastTargetControlPort)
          {
            return port;
          }
          // continue on to next port
        }
        else
        {
          RDCERR("Malformed line - expected 'n*':\n%s", &result[i]);
          return 0;
        }
      }
    }
    else
    {
      RDCERR("pid from lsof output doesn't match childPid");
      return 0;
    }
  }
  RDCERR("Failed to parse output from lsof:\n%s", result.c_str());
  return 0;
}

static bool ptrace_scope_ok()
{
  if(!FreeBSD_PtraceChildProcesses())
    return false;

  rdcstr contents;
  FileIO::ReadAll("/proc/sys/kernel/yama/ptrace_scope", contents);
  contents.trim();
  if(!contents.empty())
  {
    int ptrace_scope = atoi(contents.c_str());
    if(ptrace_scope > 1)
    {
      if(RenderDoc::Inst().IsReplayApp())
      {
        static bool warned = false;
        if(!warned)
        {
          warned = true;
          RDCWARN(
              "ptrace_scope value %d means ptrace can't be used to pause child processes while "
              "attaching.",
              ptrace_scope);
        }
      }
      return false;
    }
  }

  return true;
}

static uint64_t get_nanotime()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t ret = uint64_t(ts.tv_sec) * 1000000000ULL + uint32_t(ts.tv_nsec & 0xffffffff);
  return ret;
}

#if defined(__arm__)

// for some reason arm doesn't have the same struct name. Sigh :(
#define struct reg user_regs

#define INST_PTR_REG ARM_pc

#define BREAK_INST 0xe7f001f0ULL
#define BREAK_INST_BYTES_SIZE 4
// on ARM seemingly the instruction isn't actually considered executed, so we don't have to modify
// the instruction pointer at all.
#define BREAK_INST_INST_PTR_ADJUST 0

#elif defined(__aarch64__)

#define INST_PTR_REG pc

#define BREAK_INST 0xd4200000ULL
#define BREAK_INST_BYTES_SIZE 4
// on ARM seemingly the instruction isn't actually considered executed, so we don't have to modify
// the instruction pointer at all.
#define BREAK_INST_INST_PTR_ADJUST 0

#else

#define BREAK_INST 0xccULL
#define BREAK_INST_BYTES_SIZE 1
// step back over the instruction
#define BREAK_INST_INST_PTR_ADJUST 1

#if ENABLED(RDOC_X64)
#define INST_PTR_REG r_rip
#else
#define INST_PTR_REG r_eip
#endif

#endif

static uint64_t get_child_ip(pid_t childPid)
{
  struct reg regs = {};

  long ptraceRet = ptrace(PT_GETREGS, childPid, (caddr_t)&regs, 0);
  if(ptraceRet == 0)
    return uint64_t(regs.INST_PTR_REG);

  return 0;
}

static bool wait_traced_child(pid_t childPid, uint32_t timeoutMS, int &status)
{
  // spin waiting for the traced child, with a 100ms timeout
  status = 0;
  uint64_t start_nano = get_nanotime();
  uint64_t end_nano = 0;
  int ret = 0;

  const uint64_t timeoutNanoseconds = uint64_t(timeoutMS) * 1000 * 1000;

  while((ret = waitpid(childPid, &status, WNOHANG)) != childPid)
  {
    status = 0;

    // if we're in a capturing process then the process itself might have done waitpid(-1) and
    // swallowed the wait for our child. So as an alternative we check to see if we can query the
    // instruction pointer, which is only possible if the child is stopped.
    uint64_t ip = get_child_ip(childPid);
    if(ip != 0)
    {
      // do waitpid again in case we raced and the child stopped in between the call to waitpid and
      // get_child_ip.
      ret = waitpid(childPid, &status, WNOHANG);

      // if it still didn't succeed, set status to 0 so we know we're earlying out and don't check
      // the status codes.
      if(ret != childPid)
        status = 0;
      return true;
    }

    usleep(10);

    // check the timeout
    end_nano = get_nanotime();
    if(end_nano - start_nano > timeoutNanoseconds)
      break;
  }

  return WIFSTOPPED(status);
}

bool StopChildAtMain(pid_t childPid)
{
  // don't do this unless the ptrace scope is OK.
  if(!ptrace_scope_ok())
    return false;

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Stopping child PID %u at main", childPid);

  int childStatus = 0;

  // we have a low timeout for this stop since it should happen almost immediately (right after the
  // fork). If it didn't then we want to fail relatively fast.
  if(!wait_traced_child(childPid, 100, childStatus))
  {
    RDCERR("Didn't get initial stop from child PID %u", childPid);
    return false;
  }

  if(childStatus > 0 && WSTOPSIG(childStatus) != SIGSTOP)
  {
    RDCERR("Initial signal from child PID %u was %x, expected %x", childPid, WSTOPSIG(childStatus),
           SIGSTOP);
    return false;
  }

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Child PID %u is stopped in StopAtMainInChild()", childPid);

  int64_t ptraceRet = 0;

  // continue until exec
  struct ptrace_lwpinfo lwpinfo = {};
  lwpinfo.pl_flags = PL_FLAG_EXEC;
  ptraceRet = ptrace(PT_LWPINFO, childPid, (caddr_t)&lwpinfo, sizeof(lwpinfo));
  RDCASSERTEQUAL(ptraceRet, 0);

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Child PID %u configured to trace exec(). Continuing child", childPid);

  // continue
  // (caddr_t)1 continues where it left off (from manpage)
  ptraceRet = ptrace(PT_CONTINUE, childPid, (caddr_t)1, 0);
  RDCASSERTEQUAL(ptraceRet, 0);

  // we're not under control of when the application calls exec() after fork() in the case of child
  // processes, so be a little more generous with the timeout
  if(!wait_traced_child(childPid, 250, childStatus))
  {
    RDCERR("Didn't get to execve in child PID %u", childPid);
    return false;
  }

  if(childStatus > 0 && !(WIFSTOPPED(childStatus) || WIFEXITED(childStatus)))
  {
    RDCERR("Child PID %u exited after continue. Child status = %x", childPid, childStatus);
    return false;
  }

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Child PID %u is stopped at execve()", childPid);

  rdcstr exepath;
  long baseVirtualPointer = 0;
  uint32_t sectionOffset = 0;

  rdcstr mapsName = StringFormat::Fmt("/proc/%u/map", childPid);

  FILE *maps = FileIO::fopen(mapsName, FileIO::ReadText);

  if(!maps)
  {
    RDCERR("Couldn't open %s", mapsName.c_str());
    return false;
  }

  while(!feof(maps))
  {
    char line[512] = {0};
    if(fgets(line, 511, maps))
    {
      if(strstr(line, "r-x"))
      {
        RDCCOMPILE_ASSERT(sizeof(long) == sizeof(void *), "Expected long to be pointer sized");
	char path[512] = {0};
	// format: start, end, resident, private resident, cow, access, type, charged, charged uid.
	// this is from FreeBSD's sys/fs/procfs/procfs_map.c
	int num = sscanf(line, "0x%lx 0x%*lx %*d %*d %*p r-x %*d %*d 0x%*x %*s %*s %*s %s %*s %*d",
			&baseVirtualPointer, path);
	// there is no section offset for bsd, it looks like the start/end are what we should use
	sectionOffset = 0;

        if(num != 2)
        {
          RDCERR("Couldn't parse first executable mapping '%s'", rdcstr(line).trimmed().c_str());
          return false;
        }

        exepath = path;
        exepath.trim();
        break;
      }
    }
  }

  if(baseVirtualPointer == 0)
  {
    RDCERR("Couldn't find executable mapping in maps file");
    return false;
  }

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Child PID %u has exepath %s basePointer 0x%llx and sectionOffset 0x%x", childPid,
           exepath.c_str(), (uint64_t)baseVirtualPointer, (uint32_t)sectionOffset);

  FileIO::fclose(maps);

  FILE *elf = FileIO::fopen(exepath, FileIO::ReadText);

  if(!elf)
  {
    RDCERR("Couldn't open %s to parse ELF header", exepath.c_str());
    return false;
  }

  Elf64_Ehdr elf_header;
  size_t read = FileIO::fread(&elf_header, sizeof(elf_header), 1, elf);

  if(read != 1)
  {
    FileIO::fclose(elf);
    RDCERR("Couldn't read ELF header from %s", exepath.c_str());
    return false;
  }

  size_t entryVirtual = (size_t)elf_header.e_entry;
  // if the section doesn't shift between file offset and virtual address this will be the same
  size_t entryFileOffset = entryVirtual;

  if(elf_header.e_shoff)
  {
    if(FreeBSD_Debug_PtraceLogging())
      RDCLOG("exepath %s contains sections, rebasing to correct section", exepath.c_str());

    FileIO::fseek64(elf, elf_header.e_shoff, SEEK_SET);

    RDCASSERTEQUAL(elf_header.e_shentsize, sizeof(Elf64_Shdr));

    for(Elf64_Half s = 0; s < elf_header.e_shnum; s++)
    {
      Elf64_Shdr section_header;
      size_t read = FileIO::fread(&section_header, sizeof(section_header), 1, elf);

      if(read != 1)
      {
        FileIO::fclose(elf);
        RDCERR("Couldn't read section header from %s", exepath.c_str());
        return false;
      }

      if(section_header.sh_addr <= entryVirtual &&
         entryVirtual < section_header.sh_addr + section_header.sh_size)
      {
        if(FreeBSD_Debug_PtraceLogging())
          RDCLOG(
              "Found section in %s from 0x%llx - 0x%llx at offset 0x%llx containing entry 0x%llx.",
              exepath.c_str(), (uint64_t)section_header.sh_addr,
              uint64_t(section_header.sh_addr + section_header.sh_size),
              (uint64_t)section_header.sh_offset, (uint64_t)entryVirtual);

        entryFileOffset =
            (entryVirtual - (size_t)section_header.sh_addr) + (size_t)section_header.sh_offset;

        break;
      }
    }
  }

  FileIO::fclose(elf);

  void *entry = (void *)(baseVirtualPointer + entryFileOffset - sectionOffset);

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("child process %u executable %s has entry %p at 0x%llx + (0x%llx - 0x%x)", childPid,
           exepath.c_str(), entry, (uint64_t)baseVirtualPointer, (uint64_t)entryFileOffset,
           (uint32_t)sectionOffset);

  // this reads a 'word' and returns as long, upcast (if needed) to uint64_t
  long pio_val;
  struct ptrace_io_desc pio_desc = {};
  pio_desc.piod_op = PIOD_READ_I;
  pio_desc.piod_addr = (void *)&pio_val;
  pio_desc.piod_offs = entry;
  pio_desc.piod_len = sizeof(long);
  ptraceRet = ptrace(PT_IO, childPid, (caddr_t)&pio_desc, 0);
  RDCASSERTEQUAL(ptraceRet, 0);
  RDCASSERTEQUAL(pio_desc.piod_len, sizeof(long));
  uint64_t origEntryWord = (uint64_t)pio_val;

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Read word %llx from %p in child process %u running executable %s",
           (uint64_t)origEntryWord, entry, childPid, exepath.c_str());

  uint64_t breakpointWord =
      (origEntryWord & (0xffffffffffffffffULL << (BREAK_INST_BYTES_SIZE * 8))) | BREAK_INST;
  // downcast back to long, if that means truncating
  pio_val = (long)breakpointWord;
  pio_desc.piod_op = PIOD_WRITE_I;
  pio_desc.piod_addr = (void *)&pio_val;
  pio_desc.piod_offs = entry;
  pio_desc.piod_len = sizeof(long);
  ptraceRet = ptrace(PT_IO, childPid, (caddr_t)&pio_desc, 0);
  RDCASSERTEQUAL(ptraceRet, 0);
  RDCASSERTEQUAL(pio_desc.piod_len, sizeof(long));

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Changed word to %llx and re-poked in process %u. Continuing child",
           (uint64_t)breakpointWord, childPid);

  // continue
  ptraceRet = ptrace(PT_CONTINUE, childPid, (caddr_t)1, 0);
  RDCASSERTEQUAL(ptraceRet, 0);

  // it could take a long time to hit main so we have a large timeout here
  if(!wait_traced_child(childPid, 2000, childStatus))
  {
    RDCERR("Didn't hit breakpoint in PID %u (%x)", childPid, childStatus);
    return false;
  }

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Process %u hit entry point", childPid);

  // we're now at main! now just need to clean up after ourselves

  struct reg regs = {};

  ptraceRet = ptrace(PT_GETREGS, childPid, (caddr_t)&regs, 0);
  RDCASSERTEQUAL(ptraceRet, 0);

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Process %u instruction pointer is at %llx, for entry point %p", childPid,
           (uint64_t)(regs.INST_PTR_REG), entry);

  // step back past the byte(s) we inserted the breakpoint on
  regs.INST_PTR_REG -= BREAK_INST_INST_PTR_ADJUST;
  ptraceRet = ptrace(PT_SETREGS, childPid, (caddr_t)&regs, 0);
  RDCASSERTEQUAL(ptraceRet, 0);

  // restore the function
  pio_val = origEntryWord;
  pio_desc.piod_op = PIOD_WRITE_I;
  pio_desc.piod_addr = (void *)&pio_val;
  pio_desc.piod_offs = entry;
  pio_desc.piod_len = sizeof(long);
  ptraceRet = ptrace(PT_IO, childPid, (caddr_t)&pio_desc, 0);
  RDCASSERTEQUAL(ptraceRet, 0);
  RDCASSERTEQUAL(pio_desc.piod_len, sizeof(long));

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Process %u instruction pointer adjusted and breakpoint removed.", childPid);

  // we'll resume after reading the ident port in the calling function
  return true;
}

void StopAtMainInChild()
{
  // don't do this unless the ptrace scope is OK.
  if(!ptrace_scope_ok())
    return;

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Stopping in main at child for ptracing");

  // allow parent tracing, and immediately stop so the parent process can attach
  ptrace(PT_TRACE_ME, 0, 0, 0);

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Done PT_TRACEME, raising SIGSTOP");

  raise(SIGSTOP);

  if(FreeBSD_Debug_PtraceLogging())
    RDCLOG("Resumed after SIGSTOP");
}

void ResumeProcess(pid_t childPid, uint32_t delaySeconds)
{
  if(childPid != 0)
  {
    // if we have a delay, see if the process is paused. If so then detach it but keep it stopped
    // and wait to see if someone attaches
    if(delaySeconds > 0)
    {
      uint64_t ip = get_child_ip(childPid);

      if(ip != 0)
      {
        // detach but stop, to allow a debugger to attach
        ptrace(PT_DETACH, childPid, NULL, 0);

        rdcstr filename = StringFormat::Fmt("/proc/%u/status", childPid);

        uint64_t start_nano = get_nanotime();
        uint64_t end_nano = 0;

        const uint64_t timeoutNanoseconds = uint64_t(delaySeconds) * 1000 * 1000 * 1000;

        bool connected = false;

        // watch for a tracer to attach
        do
        {
          usleep(10);

          rdcstr status;
          FileIO::ReadAll(filename, status);

          int32_t offs = status.find("TracerPid:");

          if(offs < 0)
            break;

          status.erase(0, offs + sizeof("TracerPid:"));
          status.trim();

          end_nano = get_nanotime();

          if(status[0] != '0')
          {
            RDCLOG("Debugger PID %u attached after %f seconds", atoi(status.c_str()),
                   double(end_nano - start_nano) / 1000000000.0);
            connected = true;
            break;
          }
        } while(end_nano - start_nano < timeoutNanoseconds);

        if(!connected)
        {
          RDCLOG("Timed out waiting for debugger, resuming");
          kill(childPid, SIGCONT);
        }
        return;
      }
      else
      {
        RDCERR("Can't delay for debugger without ptrace, check ptrace_scope value");
      }
    }

    // try to detach and resume the process, ignoring any errors if we weren't tracing
    ptrace(PT_DETACH, childPid, NULL, 0);
  }
}

// because OSUtility::DebuggerPresent is called often we want it to be
// cheap. Opening and parsing a file would cause high overhead on each
// call, so instead we just cache it at startup. This fails in the case
// of attaching to processes
bool debuggerPresent = false;

void CacheDebuggerPresent()
{
  FILE *f = FileIO::fopen("/proc/self/status", FileIO::ReadText);

  if(f == NULL)
  {
    RDCWARN("Couldn't open /proc/self/status");
    return;
  }

  // read through the proc file to check for TracerPid
  while(!feof(f))
  {
    const size_t sz = 512;
    char line[sz];
    line[sz - 1] = 0;
    fgets(line, sz - 1, f);

    int tracerpid = 0;
    int num = sscanf(line, "TracerPid: %d", &tracerpid);

    // found TracerPid line
    if(num == 1)
    {
      debuggerPresent = (tracerpid != 0);
      break;
    }
  }

  FileIO::fclose(f);
}

bool OSUtility::DebuggerPresent()
{
  return debuggerPresent;
}

rdcstr Process::GetEnvVariable(const rdcstr &name)
{
  const char *val = getenv(name.c_str());
  return val ? val : rdcstr();
}

uint64_t Process::GetMemoryUsage()
{
  unsigned int ret, curpid = getpid();
  struct kinfo_proc *kip;

  kip = kinfo_getproc(curpid);
  if (kip == NULL)
       return 0;

  ret = kip->ki_rusage.ru_maxrss;

  free(kip);
  return ret;
}
