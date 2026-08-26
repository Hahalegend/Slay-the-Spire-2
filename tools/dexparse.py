#!/usr/bin/env python3
"""dexparse.py -- minimal DEX reader.

Enough of the format to answer one question: which Java classes and methods
does the native side reach through JNI, and which of those must jni_fake.c
answer? Only the index sections are parsed; bytecode is ignored entirely.

Reference: https://source.android.com/docs/core/runtime/dex-format
"""

import struct
import sys
from collections import defaultdict

ACC_STATIC = 0x0008
ACC_NATIVE = 0x0100
ACC_ABSTRACT = 0x0400
ACC_INTERFACE = 0x0200


def uleb128(buf, off):
    result = 0
    shift = 0
    while True:
        b = buf[off]
        off += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            break
        shift += 7
    return result, off


class Dex:
    def __init__(self, data):
        self.d = data
        magic = data[:8]
        if magic[:4] != b"dex\n":
            raise ValueError(f"not a dex file: {magic!r}")
        self.version = magic[4:7].decode()

        (self.string_ids_size, self.string_ids_off,
         self.type_ids_size, self.type_ids_off,
         self.proto_ids_size, self.proto_ids_off,
         self.field_ids_size, self.field_ids_off,
         self.method_ids_size, self.method_ids_off,
         self.class_defs_size, self.class_defs_off) = struct.unpack_from("<12I", data, 56)

        self._strings = {}
        self._types = {}

    def string(self, idx):
        if idx in self._strings:
            return self._strings[idx]
        off = struct.unpack_from("<I", self.d, self.string_ids_off + idx * 4)[0]
        _, off = uleb128(self.d, off)
        end = self.d.index(b"\x00", off)
        s = self.d[off:end].decode("utf-8", errors="replace")
        self._strings[idx] = s
        return s

    def type_str(self, idx):
        if idx in self._types:
            return self._types[idx]
        desc_idx = struct.unpack_from("<I", self.d, self.type_ids_off + idx * 4)[0]
        s = self.string(desc_idx)
        self._types[idx] = s
        return s

    def proto(self, idx):
        base = self.proto_ids_off + idx * 12
        shorty_idx, return_type_idx, params_off = struct.unpack_from("<3I", self.d, base)
        params = []
        if params_off:
            count = struct.unpack_from("<I", self.d, params_off)[0]
            for i in range(count):
                t = struct.unpack_from("<H", self.d, params_off + 4 + i * 2)[0]
                params.append(self.type_str(t))
        return "(" + "".join(params) + ")" + self.type_str(return_type_idx)

    def method(self, idx):
        base = self.method_ids_off + idx * 8
        class_idx, proto_idx, name_idx = struct.unpack_from("<HHI", self.d, base)
        return (self.type_str(class_idx), self.string(name_idx), self.proto(proto_idx))

    def classes(self):
        """Yields (class_descriptor, access_flags, [(name, sig, access)])."""
        for i in range(self.class_defs_size):
            base = self.class_defs_off + i * 32
            (class_idx, access_flags, superclass_idx, interfaces_off,
             source_file_idx, annotations_off, class_data_off,
             static_values_off) = struct.unpack_from("<8I", self.d, base)

            cname = self.type_str(class_idx)
            methods = []

            if class_data_off:
                off = class_data_off
                sf, off = uleb128(self.d, off)
                inf, off = uleb128(self.d, off)
                dm, off = uleb128(self.d, off)
                vm, off = uleb128(self.d, off)

                # skip fields
                for _ in range(sf + inf):
                    _, off = uleb128(self.d, off)   # field_idx_diff
                    _, off = uleb128(self.d, off)   # access_flags

                for count in (dm, vm):
                    midx = 0
                    for _ in range(count):
                        diff, off = uleb128(self.d, off)
                        acc, off = uleb128(self.d, off)
                        _, off = uleb128(self.d, off)   # code_off
                        midx += diff
                        _, mname, msig = self.method(midx)
                        methods.append((mname, msig, acc))

            yield cname, access_flags, methods


def descriptor_to_jni(desc):
    """Lcom/foo/Bar; -> com/foo/Bar"""
    if desc.startswith("L") and desc.endswith(";"):
        return desc[1:-1]
    return desc


def main():
    paths = sys.argv[1:]
    if not paths:
        print("usage: dexparse.py <classes.dex> [classes2.dex ...]")
        return 1

    all_classes = {}
    native_methods = defaultdict(list)

    for p in paths:
        with open(p, "rb") as f:
            dex = Dex(f.read())
        print(f"# {p}: dex {dex.version}, "
              f"{dex.class_defs_size} classes, {dex.method_ids_size} methods, "
              f"{dex.string_ids_size} strings", file=sys.stderr)

        for cname, cacc, methods in dex.classes():
            jni_name = descriptor_to_jni(cname)
            all_classes[jni_name] = methods
            for mname, msig, macc in methods:
                if macc & ACC_NATIVE:
                    native_methods[jni_name].append((mname, msig, macc))

    print(f"TOTAL_CLASSES {len(all_classes)}")
    print(f"CLASSES_WITH_NATIVE {len(native_methods)}")
    print()

    for cname in sorted(native_methods):
        print(f"NATIVE_CLASS {cname}")
        for mname, msig, macc in sorted(native_methods[cname]):
            flags = "static" if macc & ACC_STATIC else "instance"
            print(f"  {flags} {mname} {msig}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
