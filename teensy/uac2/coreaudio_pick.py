"""teensy/uac2/coreaudio_pick.py: pick a CoreAudio device (or a CoreMIDI port) by its exact name (and manufacturer),
as a PortAudio (sounddevice) or python-rtmidi index. The Pi 4 is "Sidecar | Expander (Pi)" (pi/engine/sidecar_gadget.h),
the Teensy "Sidecar | Expander (Mini)": both can be connected.

    from coreaudio_pick import pick, midi_pick
    dev = pick("Sidecar | Expander (Pi)")                  # or None
    port = midi_pick(None, output=True, name="Sidecar | Expander (Pi)")

Environment for the test scripts: UAC2_NAME (exact name), UAC2_MAKER (exact manufacturer). PortAudio's Core Audio
host lists the devices in kAudioHardwarePropertyDevices order; this reads the same list (name and manufacturer of
each) with ctypes, and checks that the names line up with sounddevice's before it trusts an index.
"""
import ctypes
import ctypes.util
import struct

_ca = ctypes.CDLL(ctypes.util.find_library("CoreAudio"))
_cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))


def _fourcc(s):
    return struct.unpack(">I", s.encode())[0]


class _Addr(ctypes.Structure):
    _fields_ = [("selector", ctypes.c_uint32), ("scope", ctypes.c_uint32), ("element", ctypes.c_uint32)]


_GLOBAL, _MAIN = _fourcc("glob"), 0
_cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
_cf.CFRelease.argtypes = [ctypes.c_void_p]
_ca.AudioObjectGetPropertyDataSize.argtypes = [ctypes.c_uint32, ctypes.POINTER(_Addr), ctypes.c_uint32, ctypes.c_void_p,
                                                ctypes.POINTER(ctypes.c_uint32)]
_ca.AudioObjectGetPropertyData.argtypes = [ctypes.c_uint32, ctypes.POINTER(_Addr), ctypes.c_uint32, ctypes.c_void_p,
                                            ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]


def _string(obj, sel):
    a = _Addr(_fourcc(sel), _GLOBAL, _MAIN)
    ref = ctypes.c_void_p()
    size = ctypes.c_uint32(ctypes.sizeof(ref))
    if _ca.AudioObjectGetPropertyData(obj, ctypes.byref(a), 0, None, ctypes.byref(size), ctypes.byref(ref)) or not ref:
        return ""
    buf = ctypes.create_string_buffer(512)
    ok = _cf.CFStringGetCString(ref, buf, 512, 0x08000100)   # UTF-8
    _cf.CFRelease(ref)
    return buf.value.decode() if ok else ""


def devices():
    """[(name, manufacturer)] in Core Audio's order"""
    a = _Addr(_fourcc("dev#"), _GLOBAL, _MAIN)
    size = ctypes.c_uint32()
    _ca.AudioObjectGetPropertyDataSize(1, ctypes.byref(a), 0, None, ctypes.byref(size))
    ids = (ctypes.c_uint32 * (size.value // 4))()
    _ca.AudioObjectGetPropertyData(1, ctypes.byref(a), 0, None, ctypes.byref(size), ids)
    return [(_string(i, "lnam"), _string(i, "lmak")) for i in ids]


def pick(name, maker=None):
    import sounddevice as sd
    sdn = [d["name"] for d in sd.query_devices()]
    ca = devices()
    if [n for n, _ in ca] != sdn:
        raise RuntimeError(f"Core Audio and PortAudio lists differ: {ca} / {sdn}")
    hits = [i for i, (n, m) in enumerate(ca) if n == name and (maker is None or m == maker)]
    if len(hits) > 1:
        raise RuntimeError(f"{len(hits)} devices named {name!r} (maker {maker!r}): set UAC2_MAKER")
    return hits[0] if hits else None


if __name__ == "__main__":
    for i, (n, m) in enumerate(devices()):
        print(i, n, "|", m)


# ---- CoreMIDI: the same for MIDI ports (python-rtmidi's port index = CoreMIDI's source / destination index) ----
_cm = ctypes.CDLL(ctypes.util.find_library("CoreMIDI"))
_cm.MIDIGetSource.restype = _cm.MIDIGetDestination.restype = ctypes.c_uint32
_cm.MIDIGetSource.argtypes = _cm.MIDIGetDestination.argtypes = [ctypes.c_ulong]
_cm.MIDIGetNumberOfSources.restype = _cm.MIDIGetNumberOfDestinations.restype = ctypes.c_ulong
_cm.MIDIObjectGetStringProperty.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]


def _midi_string(obj, prop):
    ref = ctypes.c_void_p()
    if _cm.MIDIObjectGetStringProperty(obj, ctypes.c_void_p.in_dll(_cm, prop), ctypes.byref(ref)) or not ref:
        return ""
    buf = ctypes.create_string_buffer(512)
    ok = _cf.CFStringGetCString(ref, buf, 512, 0x08000100)
    _cf.CFRelease(ref)
    return buf.value.decode() if ok else ""


def midi_ports(output):
    """[(name, manufacturer)] of the MIDI destinations (output=True) or sources, in CoreMIDI's (= rtmidi's) order"""
    n = _cm.MIDIGetNumberOfDestinations() if output else _cm.MIDIGetNumberOfSources()
    get = _cm.MIDIGetDestination if output else _cm.MIDIGetSource
    return [(_midi_string(get(i), "kMIDIPropertyDisplayName"), _midi_string(get(i), "kMIDIPropertyManufacturer"))
            for i in range(n)]


def midi_pick(maker, output, name=None):
    """the port of that manufacturer and/or display name (None: any)"""
    hits = [i for i, (n, m) in enumerate(midi_ports(output))
            if (maker is None or m == maker) and (name is None or n == name)]
    if len(hits) > 1:
        raise RuntimeError(f"{len(hits)} MIDI ports named {name!r} (maker {maker!r})")
    return hits[0] if hits else None
