#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise Aravis through a local GigE Vision camera simulator."""

import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from tempfile import TemporaryDirectory
from uuid import uuid4


@contextmanager
def camera_on_loopback(fake_camera, arv_tool, serial, genicam=None):
    device = f"Aravis-Fake-{serial}"
    arguments = [fake_camera, "-i", "127.0.0.1", "-s", serial]
    if genicam is not None:
        arguments += ["-g", str(genicam)]
    camera = subprocess.Popen(arguments)
    try:
        for _ in range(3):
            if camera.poll() is not None:
                raise RuntimeError(f"Fake camera exited with status {camera.returncode}")
            discovery = subprocess.check_output([arv_tool], text=True, timeout=12)
            if any(line.startswith(f"{device} (127.0.0.1)") for line in discovery.splitlines()):
                break
        else:
            raise RuntimeError(f"Fake camera {device} was not discovered on loopback\n{discovery}")
        yield device
    finally:
        camera.terminate()
        try:
            camera.wait(timeout=2)
        except subprocess.TimeoutExpired:
            camera.kill()
            camera.wait()


def add_control_fixtures(xml):
    # TestBoolean already maps to the fake camera's TestRegister.
    debug = "<pFeature>TestRegister</pFeature>"
    for feature in ("TestBoolean", "TestLabel", "TestUnavailableLabel",
                    "TestUnavailableInteger", "TestUnavailableFloat", "TestMode"):
        debug += f"\n        <pFeature>{feature}</pFeature>"
    xml = xml.replace("<pFeature>TestRegister</pFeature>", debug, 1)
    controls = """
    <Integer Name="TestUnavailable">
      <Value>0</Value>
    </Integer>
    <String Name="TestLabel">
      <Value>Original</Value>
    </String>
    <String Name="TestUnavailableLabel">
      <pIsAvailable>TestUnavailable</pIsAvailable>
      <Value>Hidden</Value>
    </String>
    <Integer Name="TestUnavailableInteger">
      <pIsAvailable>TestUnavailable</pIsAvailable>
      <Value>1</Value>
    </Integer>
    <Float Name="TestUnavailableFloat">
      <pIsAvailable>TestUnavailable</pIsAvailable>
      <Value>1.0</Value>
    </Float>
    <Enumeration Name="TestMode">
      <EnumEntry Name="Active"><Value>0</Value></EnumEntry>
      <EnumEntry Name="Hidden">
        <pIsAvailable>TestUnavailable</pIsAvailable>
        <Value>1</Value>
      </EnumEntry>
      <Value>0</Value>
    </Enumeration>
    """
    return xml.replace("</RegisterDescription>", controls + "</RegisterDescription>", 1)


def main():
    fake_camera, arv_tool, camera_test, capture_test, plugin, faults = sys.argv[1:]
    serial = "PW" + uuid4().hex[:6]
    with camera_on_loopback(fake_camera, arv_tool, serial) as device:
        raw_xml = subprocess.check_output([arv_tool, "-n", device, "genicam"], timeout=10)
        # The command may include bytes past the XML document.
        closing_tag = b"</RegisterDescription>"
        start, end, _ = raw_xml.partition(closing_tag)
        if not end:
            raise RuntimeError("Simulator did not return a complete GenICam document")
        stock_xml = (start + end).decode("utf-8")

    with TemporaryDirectory() as directory:
        genicam = Path(directory) / "genicam.xml"
        genicam.write_text(add_control_fixtures(stock_xml), encoding="utf-8")
        serial = "PW" + uuid4().hex[:6]
        with camera_on_loopback(fake_camera, arv_tool, serial, genicam) as device:
            subprocess.run([arv_tool, "-n", device, "control", "Width=64", "AcquisitionFrameRate=12"],
                           check=True, timeout=10)
            subprocess.run([camera_test, device, "native-gv", "loopback"], check=True, timeout=15)
            subprocess.run([capture_test, plugin, device, "frame", "loopback"], check=True, timeout=15)
            subprocess.run([arv_tool, "-n", device, "control", "AcquisitionFrameRate=5"], check=True, timeout=10)
            subprocess.run([capture_test, plugin, device, "row-block", "loopback"], check=True, timeout=15)
            subprocess.run([capture_test, faults, device, "frame", "faults"], check=True, timeout=30)
            subprocess.run([capture_test, faults, device, "row-block", "faults"], check=True, timeout=15)


if __name__ == "__main__":
    main()
