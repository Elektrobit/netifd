import os
import time
import pyroute2
import subprocess
import json
from logging import Logger
from tempfile import TemporaryDirectory
from pyroute2 import IPRoute, NetNS
import pyroute2.netns
from typing import List

from .loader import TestSuite, InterfaceFile
from .writer import ResultWriter
from .compare import Compare
from .process import start_process, run_process


UBUSD_PATH = "/opt/netifd/sbin/ubusd"
UBUS_PATH = "/opt/netifd/bin/ubus"
DHCPD_PATH = "/usr/sbin/dhcpd"
NETIFD_PATH = "/opt/netifd/sbin/netifd"


class Timer():
    _duration: float
    _end: float
    def __init__(self, sec: float) -> None:
        self._duration = sec
        self._end = time.monotonic() + self._duration

    def reset(self) -> None:
        self._end = time.monotonic() + self._duration

    @property
    def expired(self) -> bool:
        if self._end != 0:
            if time.monotonic() >= self._end:
                self._end = 0
        return self._end == 0

class TestSuiteRun():
    _logger: Logger
    _rw: ResultWriter
    _suite: TestSuite
    _netns_test: NetNS = None
    _ipr: IPRoute = None
    _processes: List[subprocess.Popen] = None
    _veths: List[int] = None
    _tempdir: TemporaryDirectory = None

    def __init__(self, logger: Logger, result_writer: ResultWriter, suite: TestSuite) -> None:
        self._logger = logger
        self._rw = result_writer
        self._suite = suite
        self._ipr = IPRoute()
        self._processes = []
        self._veths = []

    def _add_veth_pair(self, name: str, peername: str, peermac: str = None) -> int:
        self._ipr.link('add',
            ifname=name,
            kind='veth',
            peer={
                "ifname": peername,
                "net_ns_fd": self._netns_test.status["netns"]
            }
        )
        idx = self._ipr.link_lookup(ifname=name)[0]
        self._ipr.link('set', index=idx, state='up')
        if peermac:
            self._netns_test.link('set', ifname=peername, address=peermac)
        self._veths.append(idx)
        return idx

    def _get_temp_file(self, name: str) -> str:
        return os.path.join(self._tempdir.name, name)

    def _setup_dummy_eth0(self) -> None:
        self._add_veth_pair('netifd_eth0', 'eth0', '02:eb:eb:eb:eb:eb')

    def _setup_dhcp_servers(self) -> None:
        interfaces = {}

        for config in self._suite.dhcp_config:
            if config.interface not in interfaces:
                interfaces[config.interface] = self._add_veth_pair("netifd_" + config.interface, config.interface)
            if config.addr:
                self._ipr.addr('add',
                    index = interfaces[config.interface],
                    address = config.addr.address,
                    mask = config.addr.netmask
                )

            basename = self._get_temp_file(f"dhcpd{config.version}_{config.interface}")
            lf = basename + ".lease"
            log = basename + ".log"
            with open(lf, "w") as f: pass
            self._start_process([
                DHCPD_PATH,
                "-d",
                f"-{config.version}",
                "-f",
                "-cf", config.path,
                "-lf", lf,
                "netifd_" + config.interface
            ], log = log)

    def _start_process(self, cmd: List[str], netns: NetNS = None, log: str = None) -> None:
        self._processes.append(start_process(cmd, netns, log))

    def _start_ubusd(self) -> None:
        self._start_process([UBUSD_PATH], self._netns_test)

    def _start_netifd(self) -> None:
        # We have to start netifd using ip netns exec,
        # otherwise /sys is not remounted and netifd
        # does not work correctly
        self._start_process(
            [
                "ip",
                "netns",
                "exec",
                self._netns_test.status["netns"],
                NETIFD_PATH,
                "-c", os.path.dirname(self._suite.network_config),
                "-r", self._get_temp_file("resolv.conf"),
                "-S",
                "-p", "/",
                "-l", "4"
            ],
            log = self._get_temp_file("netifd.log")
        )

    def _wait_for_ubus(self) -> None:
        t = Timer(1)
        while True:
            res = run_process(
                [UBUS_PATH, "list"], 
                self._netns_test, 
                stdout=subprocess.DEVNULL, 
                stderr=subprocess.DEVNULL
            )
            if res['rc'] == 0:
                break
            time.sleep(0.5)
            if t.expired:
                raise TimeoutError("Timeout waiting for ubus")

    def _wait_for_network(self) -> bool:
        return run_process(
            [
                UBUS_PATH,
                "wait_for",
                "network"
            ],
            self._netns_test,
            stdout = subprocess.DEVNULL,
            stderr = subprocess.DEVNULL
        )['rc'] == 0

    def _call_ubus(self, args: List[str]):
        res = run_process(
            [
                UBUS_PATH,
                *args
            ],
            self._netns_test,
            stdout = subprocess.PIPE,
            stderr = subprocess.STDOUT
        )
        return (res['rc'] == 0, res['stdout'])

    def _wait_for_interfaces(self) -> None:
        remaining = self._suite.waitfor_interfaces

        if not remaining:
            return

        timer = Timer(15)
        self._logger.debug("Waiting for interfaces to come up: " + ", ".join(remaining))
        while remaining:
            for intf in remaining[:]:
                res = self._call_ubus([
                    "call",
                    f"network.interface.{intf}",
                    "status"
                ])
                if res[0]:
                    res = json.loads(res[1])
                    if res["up"]:
                        self._logger.debug(f"Interface {intf} is up")
                        remaining.remove(intf)
            time.sleep(0.5)
            if timer.expired:
                raise TimeoutError("Timeout waiting for interfaces: " + ", ".join(remaining))

    def _make_dummy_sysmount(self) -> None:
        # ip netns exec tries to mount /sys in the namespace.
        # In order to do that it lazy umounts /sys first.
        # If this is the only mounted sysfs, further attempts to mount sys fail no with EPERM
        # Just mounting another sysfs somewhere else resolves the problem
        path = self._get_temp_file("dummy_sys_mount")
        os.mkdir(path)
        os.system(f"mount -t sysfs none {path}")

    def start(self):
        self._tempdir = TemporaryDirectory()
        self._make_dummy_sysmount()
        self._netns_test = NetNS("test")
        self._setup_dummy_eth0()
        self._setup_dhcp_servers()

        self._start_ubusd()
        self._logger.debug("Waiting for ubus to start")
        self._wait_for_ubus()
        self._start_process(["/opt/netifd/bin/ubus", "monitor"], self._netns_test, self._get_temp_file("ubus.monitor"))
        self._logger.debug("Starting netifd")
        self._start_netifd()
        if not self._wait_for_network():
            raise TimeoutError("Timeout waiting for netfid")

        self._wait_for_interfaces()

    def _validate_nameserver(self, expected: str) -> None:
        with open(expected, "r") as f:
            expected_servers = set([l.strip() for l in f.readlines() if l.strip()])

        with open(self._get_temp_file("resolv.conf"), "r") as f:
            actual_servers = set([
                l.split(" ")[1].strip()
                    for l in f.readlines()
                    if l.startswith("nameserver")
            ])
        missing_servers = expected_servers - actual_servers
        unexpected_servers = actual_servers - expected_servers

        messages = []
        if missing_servers:
            messages.append("Nameservers are missing: " + ", ".join(missing_servers))
        if unexpected_servers:
            messages.append("Nameservers are not expected: " + ", ".join(unexpected_servers))

        if messages:
            self._rw.fail("\n".join(messages))

    def _validate_ip(self, cmd: List[str], val_file: InterfaceFile, select: str = None):
        outfile = self._get_temp_file(val_file.name + ".out")
        with self._rw.start_suite(val_file.name):
            with self._rw.start_test("Setup"):
                res = run_process(
                    [
                        'ip',
                        f'-{val_file.version}' if val_file.version != 0 else '',
                        '-j',
                        *cmd
                    ],
                    self._netns_test,
                    stdout=outfile,
                    stderr=subprocess.PIPE
                )
                if res["rc"] != 0:
                    self._rw.fatal("Error getting current config: " + res["stderr"])
                    return

                with open(outfile) as f:
                    actual = json.load(f)
        
            with open(val_file.path) as f:
                expected = json.load(f)

            cmp = Compare(self._rw)
            cmp.compare(expected, actual, select)  

    def _validate_ip_addr(self, val_file: InterfaceFile):
        args = [
            'addr', 'show',
            'dev', val_file.interface,
        ]
        if val_file.version == 6:
            args += ['scope', 'global']
        self._validate_ip(args, val_file, 'addr_info')

    def _validate_ip_link(self, val_file: InterfaceFile):
        self._validate_ip([
            '-d', 
            'link', 'show',
            'dev', val_file.interface
        ], val_file)

    def _validate_ip_route(self, val_file: InterfaceFile):
        self._validate_ip([
            'route', 'show',
            'dev', val_file.interface,
            'protocol', 'static'
        ], val_file)

    def _validate_sysctl(self, val_file: InterfaceFile):
        with open(val_file.path, "r") as f:
            expected = json.load(f)

        with self._rw.start_suite(val_file.name):
            interface = val_file.interface
            if interface == "global":
                interface = ".."
            
            path = f"/proc/sys/net/ipv{val_file.version}/conf/{interface}/"

            for key, value in expected.items():
                with self._rw.start_test(key):
                    res = run_process(["cat", os.path.join(path, key)], self._netns_test, stdout=subprocess.PIPE)
                    if res['rc'] != 0:
                        self._rw.fatal("Cannot access sysctl file")
                    else:
                        actual = res["stdout"]
                        if value != actual:
                            self._rw.fail(f"Value '{actual}' does not match expected value '{value}")


    def validate(self):
        FILE_TO_FUNC = {
            "ipaddr4": "_validate_ip_addr",
            "ipaddr6": "_validate_ip_addr",
            "iplink": "_validate_ip_link",
            "iproute4": "_validate_ip_route",
            "iproute6": "_validate_ip_route",
            "sysctl4": "_validate_sysctl",
            "sysctl6": "_validate_sysctl",
        }
        for val_file in self._suite.validation_files:
            if val_file.type == "nameservers":
                with self._rw.start_test("Nameserver"):
                    self._validate_nameserver(val_file.path)
            elif val_file.type in FILE_TO_FUNC:
                getattr(self, FILE_TO_FUNC[val_file.type])(val_file)
            else:
                raise NotImplemented(val_file.path)

    def shell_in_ns(self) -> None:
        pyroute2.netns.pushns("test")
        os.system("bash")
        pyroute2.netns.popns()

    def cleanup(self):
        with self._rw.start_test("Teardown"):
            for veth in self._veths:
                self._ipr.link('del', index = veth)
            self._veths = []
            if self._netns_test:
                self._netns_test.remove()
                self._netns_test.close()
                self._netns_test = None
            for process in reversed(self._processes):
                try:
                    process.terminate()
                    try:
                        process.wait(3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    if hasattr(process, "release"):
                        process.release()
                except:
                    pass
            self._processes = []
            if self._tempdir:
                path = self._get_temp_file("dummy_sys_mount")
                if os.path.isdir(path):
                    os.system(f"umount {path}")
                self._tempdir.cleanup()
                self._tempdir = None

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.cleanup()


class TestRunner():
    _logger: Logger
    suites: List[TestSuite] = None

    def __init__(self, logger: Logger, testsdir: str) -> None:
        self._logger = logger
        self._load_suites(testsdir)

    def _load_suites(self, testsdir: str) -> None:
        suites = []
        for entry in sorted(os.listdir(testsdir)):
            fullpath = os.path.join(testsdir, entry)
            if os.path.isdir(fullpath) and entry.startswith("test_"):
                suites.append(TestSuite(fullpath))
        self.suites = suites

    def run(self, result_writer: ResultWriter, suites: List[TestSuite] = None, shell = False):
        suites = suites or self.suites
        try:
            for suite in suites:
                with result_writer.start_suite(suite.name):
                    with TestSuiteRun(self._logger, result_writer, suite) as run:
                        with result_writer.start_test("Setup"):
                            try:
                                run.start()
                            except TimeoutError as e:
                                result_writer.fatal("Timeout: " + e.args[0])
                            if shell:
                                run.shell_in_ns()
                        run.validate()
        except KeyboardInterrupt:
            raise

    def get_suite(self, name: str) -> TestSuite:
        if name.startswith("test_"):
            name = name[5:]
        return next(filter(lambda x: x.name == name, self.suites), None)
