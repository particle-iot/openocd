#!/usr/bin/env python3

import sys
import os


def create_header(base_name: str, function_name: str,
                  struct_type: str, code: str):
    include_protection = "_CONTRIB_GPERF_{}".format(base_name.upper())
    function_decl = "struct {} * {} (register const char *str, register unsigned int len);".format(
        struct_type, function_name
    )

    header = """
#ifndef {0}
#define {0}

#include <stdlib.h>
#include <string.h>

/* Generated by gperf_convert.py. Do not edit directly. */

{1}

{2}

#endif // {0}
""".format(include_protection, code, function_decl)

    return header


def read_file(name: str):
    file = open(name, 'r')
    contents = file.read()
    file.close()
    return contents


def write_file(name: str, contents: str):
    file = open(name, 'w')
    file.write(contents)
    file.close()


def extract_struct_type(s: str):
    token = "struct"
    pos = s.find(token)

    if pos == -1:
        raise Exception('Could not find struct definition')

    pos_start = pos + len(token)
    pos_end = s.find("{", pos_start)

    struct_type = s[pos_start:pos_end].strip()
    return struct_type


def extract_function_name(s: str):
    token = "%define lookup-function-name"
    pos = s.find(token)

    if pos == -1:
        raise Exception('Could not find lookup-function-name definition')

    pos_start = pos + len(token)
    pos_end = s.find("\n", pos_start)

    function_name = s[pos_start:pos_end].strip()
    return function_name


def extract_code(start_marker: str, end_marker: str, s: str):
    start_pos = s.find(start_marker)

    if start_pos == -1:
        raise Exception('Could not parse gperf code')

    end_pos = s.find(end_marker, start_pos)

    if end_pos == -1:
        raise Exception('Could not parse gperf code')

    return s[start_pos + len(start_marker):end_pos]


def main(argv):
    gperf_file_name = argv[0]
    in_file_name = argv[1]
    out_file_name = argv[2]

    base_name = os.path.splitext(out_file_name)[0]
    include_file = os.path.basename(out_file_name)
    include_string = "#include \"" + include_file + "\"\n"

    gperf_contents = read_file(gperf_file_name)
    in_contents = read_file(in_file_name)

    enum_definition = extract_code("%{", "%}", gperf_contents)
    struct_definition = extract_code("%}", "%%", gperf_contents)
    function_name = extract_function_name(gperf_contents)
    struct_type = extract_struct_type(struct_definition)

    in_contents = in_contents.replace(enum_definition, include_string)
    in_contents = in_contents.replace(struct_definition, '')

    code = enum_definition + "\n\n" + struct_definition
    code = create_header(base_name, function_name, struct_type, code)

    write_file(in_file_name, in_contents)
    write_file(out_file_name, code)


if __name__ == "__main__":
    if len(sys.argv) != 3 + 1:
        print("usage: convert.py <input .gperf> "
              "<input source> <output header>")
        sys.exit(1)
    main(sys.argv[1:])

