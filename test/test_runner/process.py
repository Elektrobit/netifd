import os
import subprocess
from pyroute2 import NetNS, NSPopen
from typing import List

def start_process(cmd: List[str], netns: NetNS = None, log: str = None) -> subprocess.Popen:
    args = dict(
        args = cmd,
        shell = False
    )

    fd = -1
    if log:
        fd = os.open(log, os.O_RDWR | os.O_CREAT)
        args["stderr"] = subprocess.STDOUT
        args["stdout"] = fd
    else:
        args["stderr"] = args["stdout"] = subprocess.DEVNULL

    if not netns:
        process = subprocess.Popen(**args)
    else:
        process = NSPopen(netns.status["netns"], **args)
    if fd != -1:
        os.close(fd)
    return process

def run_process(cmd: List[str], netns: NetNS = None, stdout: str = None, stderr: str = None, shell = False) -> dict:
    if not netns: raise NotImplementedError()

    args = dict(
        args = cmd,
        shell = shell
    )

    stdout_fd = -1
    if isinstance(stdout, str):
        stdout_fd = os.open(stdout, os.O_RDWR | os.O_CREAT)
        args["stdout"] = stdout_fd
    elif stdout is not None:
        args["stdout"] = stdout

    stderr_fd = -1
    if isinstance(stderr, str):
        stderr_fd = os.open(stderr, os.O_RDWR | os.O_CREAT)
        args["stderr"] = stderr_fd
    elif stderr is not None:
        args["stderr"] = stderr

    proc = NSPopen(
        netns.status["netns"],
        **args
    )
    out = proc.communicate()
    rc = proc.wait()
    try:
        proc.release()
    except OSError as e:
        # Sometimes release raises a EBADFD
        pass

    if stdout_fd != -1:
        os.close(stdout_fd)
    if stderr_fd != -1:
        os.close(stderr_fd)

    res = {
        'rc': rc
    }
    if stdout == subprocess.PIPE:
        res['stdout'] = out[0].decode(errors="replace").strip()
    if stderr == subprocess.PIPE:
        res['stderr'] = out[1].decode(errors="replace").strip()

    return res
