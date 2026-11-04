"""pi/circle/bootimg.py on a small FAT32 image made by mkfs.fat and filled by mtools (skipped without them)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "circle", "bootimg.py")
sys.path.insert(0, os.path.join(HERE, "..", "..", "circle"))
import sdboot  # noqa: E402

def put(path, data):
    with open(path, "wb") as f:
        f.write(data)


TOOLS = all(shutil.which(t) for t in ("mkfs.fat", "mcopy", "mtype"))


@unittest.skipUnless(TOOLS, "needs mkfs.fat and mtools")
class BootImg(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.img = os.path.join(self.d, "boot.img")
        with open(self.img, "wb") as f:
            f.truncate(48 << 20)
        subprocess.run(["mkfs.fat", "-F", "32", "-s", "1", self.img], check=True, capture_output=True)
        self.env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        for name, data in (("cmdline.txt", b"sdport=0\n"), ("engine.img", sdboot.wrap(os.urandom(300000)))):
            p = os.path.join(self.d, name)
            put(p, data)
            subprocess.run(["mcopy", "-i", self.img, p, "::" + name], check=True, env=self.env)

    def tearDown(self):
        shutil.rmtree(self.d)

    def run_tool(self, *a):
        return subprocess.run([sys.executable, TOOL, self.img, *a], capture_output=True, text=True)

    def mtype(self, name):
        return subprocess.run(["mtype", "-i", self.img, "::" + name], check=True, capture_output=True,
                              env=self.env).stdout

    def test_cmdline_and_kernel(self):
        kernel = os.urandom(123457)
        kp = os.path.join(self.d, "kernel8-rpi4.img")
        put(kp, kernel)
        r = self.run_tool("--cmdline", "sdport=0 link=tcp:192.168.1.2:7302", "--kernel", kp)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.mtype("cmdline.txt"), b"sdport=0 link=tcp:192.168.1.2:7302\n")
        self.assertEqual(self.mtype("engine.img"), sdboot.wrap(kernel))
        if shutil.which("fsck.fat"):
            r = subprocess.run(["fsck.fat", "-n", self.img], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_refuses_what_does_not_fit_or_is_not_wrapped(self):
        big = os.path.join(self.d, "big.img")
        put(big, sdboot.wrap(os.urandom(400000)))
        self.assertNotEqual(self.run_tool("--engine", big).returncode, 0)
        raw = os.path.join(self.d, "raw.img")
        put(raw, os.urandom(1000))
        self.assertNotEqual(self.run_tool("--engine", raw).returncode, 0)
        self.assertEqual(self.mtype("cmdline.txt"), b"sdport=0\n")


if __name__ == "__main__":
    unittest.main()
