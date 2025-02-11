/* **********************************************************
 * Copyright (c) 2023 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* DrPT2Trace.c
 * A command-line tool that decodes a PT trace, transforms it into a memtrace made up of
 * trace_entry_t records, and outputs all records.
 * This standalone client is not a component of the drmemtrace/drcachesim workflow.
 * Instead, it is utilized for converting either the PT trace generated by the "perf
 * record" command or a single PT raw trace file produced by "drcachesim".
 */

#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <memory>
#include <unistd.h>

#include "droption.h"
#include "intel-pt.h"
#include "pt2ir.h"
#include "ir2trace.h"
#include "trace_entry.h"

#define CLIENT_NAME "drpt2trace"
#define SUCCESS 0
#define FAILURE 1

#if !defined(X86_64) || !defined(LINUX)
#    error "This is only for Linux x86_64."
#endif

/***************************************************************************
 * Options
 */

/* A collection of options. */
static droption_t<bool> op_help(DROPTION_SCOPE_FRONTEND, "help", false,
                                "Print this message", "Prints the usage message.");
static droption_t<bool> op_print_instrs(DROPTION_SCOPE_FRONTEND, "print_instrs", false,
                                        "Print instructions.",
                                        "Print the disassemble code of the trace.");
static droption_t<std::string> op_mode(
    DROPTION_SCOPE_FRONTEND, "mode", "",
    "[Required] The mode for decoding the trace. Valid modes are: ELF, SIDEBAND",
    "Specifies the mode for decoding the trace. Valid modes are:\n"
    "ELF: The raw bits of this PT trace are all in one ELF file. \n"
    "SIDEBAND: The raw bits of this PT trace are in different image files, and the "
    "sideband data contains the image switching info that can be used in the decoding "
    "process. \n");

static droption_t<std::string>
    op_raw_pt(DROPTION_SCOPE_FRONTEND, "raw_pt", "",
              "[Required] Path to the PT raw trace file",
              "Specifies the file path of the PT raw trace. Please run the "
              "libipt/script/perf-read-aux.bash script to get PT raw trace file from the "
              "data generated by the perf record command.");
static droption_t<std::string> op_raw_pt_format(
    DROPTION_SCOPE_FRONTEND, "raw_pt_format", "",
    "[Required] The format of the input raw PT. Valid formats are: PERF, DRMEMTRACE",
    "Specifies the format of the input raw PT. Valid formats are:\n"
    "PERF: The input raw PT is generated by perf command. \n"
    "DRMEMTRACE: The input raw PT is generated by drmemtrace/drcachesim.");

static droption_t<std::string>
    op_elf(DROPTION_SCOPE_FRONTEND, "elf", "", "[Optional] Path to the ELF file",
           "This is a required option in ELF Mode. Specifies the file path of the ELF "
           "file. This must be specified when converting traces that don't contain "
           "sideband information. e.g. kernel-only traces and short user traces.");
static droption_t<unsigned long long> op_elf_base(
    DROPTION_SCOPE_FRONTEND, "elf_base", 0x0,
    "[Optional] The runtime load address of the elf file",
    "This is an optional option in elf_base Mode. Specifies the runtime load address of "
    "the elf file. For kernel cases, this always should be 0x0, so it is not required. "
    "But if -elf specified file's runtime load address is not 0x0, it must be set.");

static droption_t<std::string>
    op_primary_sb(DROPTION_SCOPE_FRONTEND, "primary_sb", "",
                  "[Optional] Path to primary sideband stream file",
                  "Specifies the file path of the primary sideband stream. A primary "
                  "sideband file is directly related to the trace.  For example, it may "
                  "contain the sideband information for the traced cpu. Please run the "
                  "libipt/script/perf-read-sideband.bash script to get PT sideband file "
                  "from the data generated by the perf record command. This must be "
                  "specified when converting traces that the instruction bytes are "
                  "located in multiple images. e.g., the traces of the application that "
                  "load and unload images dynamically. ");
static droption_t<std::string> op_secondary_sb(
    DROPTION_SCOPE_FRONTEND, "secondary_sb", DROPTION_FLAG_ACCUMULATE, "",
    "[Optional] Path to secondary sideband stream file",
    "This is an optional option in SIDEBAND Mode. Specifies the file path of the "
    "secondary sideband stream. A secondary sideband file "
    "is indirectly related to the trace.  For example, it may contain the sideband "
    "information for other cpus on the system. Please "
    "run the libipt/script/perf-read-sideband.bash script to get PT "
    "sideband file from the data generated by the perf record command.");
static droption_t<std::string> op_sb_kcore_path(
    DROPTION_SCOPE_FRONTEND, "sb_kcore_path", "", "[Optional] Path to kcore file",
    "This is an optional option in SIDEBAND Mode. Specifies the file path of "
    "kernel's core dump file. To get the kcore file, "
    "please use 'perf record --kcore' to record PT raw trace.");

/* Below options are required by the libipt and libipt-sb.
 * XXX: We should use a config file to specify these options and parse the file in pt2ir.
 */
static droption_t<int> op_pt_cpu_family(
    DROPTION_SCOPE_FRONTEND, "pt_cpu_family", 0,
    "[libipt Optional] set cpu family for PT raw trace",
    "Set cpu family to the given value. Please run the "
    "libipt/script/perf-get-opts.bash script to get the value of this option "
    "from the data generated by the perf record command.");
static droption_t<int> op_pt_cpu_model(
    DROPTION_SCOPE_FRONTEND, "pt_cpu_model", 0,
    "[libipt Optional] set cpu model for PT raw trace",
    "Set cpu model to the given value. Please run the "
    "libipt/script/perf-get-opts.bash script to get the value of this option "
    "from the data generated by the perf record command.");
static droption_t<int> op_pt_cpu_stepping(
    DROPTION_SCOPE_FRONTEND, "pt_cpu_stepping", 0,
    "[libipt Optional] set cpu stepping for PT raw trace",
    "Set cpu stepping to the given value. Please run the "
    "libipt/script/perf-get-opts.bash script to get the value of this option "
    "from the data generated by the perf record command.");
static droption_t<int>
    op_pt_mtc_freq(DROPTION_SCOPE_FRONTEND, "pt_mtc_freq", 0,
                   "[libipt Optional] set mtc frequency for PT raw trace",
                   "Set mtc frequency to the given value. Please run the "
                   "libipt/script/perf-get-opts.bash script to get the value of this "
                   "option from the data generated by the perf record command.");
static droption_t<int>
    op_pt_nom_freq(DROPTION_SCOPE_FRONTEND, "pt_nom_freq", 0,
                   "[libipt Optional] set nom frequency for PT raw trace",
                   "Set nom frequency to the given value. Please run the "
                   "libipt/script/perf-get-opts.bash script to get the value of this "
                   "option from the data generated by the perf record command.");
static droption_t<int> op_pt_cpuid_0x15_eax(
    DROPTION_SCOPE_FRONTEND, "pt_cpuid_0x15_eax", 0,
    "[libipt Optional] set the value of cpuid[0x15].eax for PT raw trace",
    "Set the value of cpuid[0x15].eax to the given value. Please run the "
    "libipt/script/perf-get-opts.bash script to get the value of this option from the "
    "data generated by the perf record command.");
static droption_t<int> op_pt_cpuid_0x15_ebx(
    DROPTION_SCOPE_FRONTEND, "pt_cpuid_0x15_ebx", 0,
    "[libipt Optional] set the value of cpuid[0x15].ebx for PT raw trace",
    "Set the value of cpuid[0x15].ebx to the given value. Please run the "
    "libipt/script/perf-get-opts.bash script to get the value of this option from the "
    "data generated by the perf record command.");
static droption_t<unsigned long long>
    op_sb_sample_type(DROPTION_SCOPE_FRONTEND, "sb_sample_type", 0x0,
                      "[libipt-sb Required] set sample type for sideband stream",
                      "Set sample type to the given value(the given value must be a "
                      "hexadecimal integer and default: 0x0). Please run the "
                      "libipt/script/perf-get-opts.bash script to get the value of this "
                      "option from the data generated by the perf record command.");
static droption_t<std::string>
    op_sb_sysroot(DROPTION_SCOPE_FRONTEND, "sb_sysroot", "",
                  "[libipt-sb Optional] set sysroot for sideband stream",
                  "Set sysroot to the given value. Please run the "
                  "libipt/script/perf-get-opts.bash script to get the value of this "
                  "option from the data generated by the perf record command.");
static droption_t<unsigned long long>
    op_sb_time_zero(DROPTION_SCOPE_FRONTEND, "sb_time_zero", 0,
                    "[libipt-sb Optional] set time zero for sideband stream",
                    "Set perf_event_mmap_page.time_zero to the given value. Please run "
                    "the libipt/script/perf-get-opts.bash script to get the value of "
                    "this option from the data generated by the perf record command.");
static droption_t<unsigned int>
    op_sb_time_shift(DROPTION_SCOPE_FRONTEND, "sb_time_shift", 0,
                     "[libipt-sb Optional] set time shift for sideband stream",
                     "Set perf_event_mmap_page.time_shift to the given value. Please run "
                     "the libipt/script/perf-get-opts.bash script to get the value of "
                     "this option from the data generated by the perf record command.");
static droption_t<unsigned int>
    op_sb_time_mult(DROPTION_SCOPE_FRONTEND, "sb_time_mult", 1,
                    "[libipt-sb Optional] set time mult for sideband stream",
                    "Set perf_event_mmap_page.time_mult to the given value. Please run "
                    "the libipt/script/perf-get-opts.bash script to get the value of "
                    "this option from the data generated by the perf record command.");
static droption_t<unsigned long long>
    op_sb_tsc_offset(DROPTION_SCOPE_FRONTEND, "sb_tsc_offset", 0x0,
                     "[libipt-sb Optional] set tsc offset for sideband stream",
                     "Set perf events the given value ticks earlier(the given value "
                     "must be a hexadecimal integer and default: 0x0). Please run the "
                     "libipt/script/perf-get-opts.bash script to get the value of this "
                     "option from the data generated by the perf record command.");
static droption_t<unsigned long long> op_sb_kernel_start(
    DROPTION_SCOPE_FRONTEND, "sb_kernel_start", 0x0,
    "[libipt-sb Optional] set kernel start for sideband stream",
    "Set the start address of the kernel to the given value(the "
    "given value must be a hexadecimal integer and default: 0x0). Please run the "
    "libipt/script/perf-get-opts.bash script to get the value of this option from the "
    "data generated by the perf record command.");

/* The auto cleanup wrapper of struct pt_image_section_cache.
 * This can ensure the instance of pt_image_section_cache is cleaned up when it is out of
 * scope.
 */
struct pt_iscache_autoclean_t {
public:
    pt_iscache_autoclean_t(struct pt_image_section_cache *iscache)
        : iscache(iscache)
    {
    }

    ~pt_iscache_autoclean_t()
    {
        if (iscache != nullptr) {
            pt_iscache_free(iscache);
            iscache = nullptr;
        }
    }

    struct pt_image_section_cache *iscache = nullptr;
};

/****************************************************************************
 * Output
 */

static void
print_results(IN drir_t &drir, IN std::vector<trace_entry_t> &entries)
{
    if (drir.get_ilist() == nullptr) {
        std::cerr << "The list to store decoded instructions is not initialized."
                  << std::endl;
        return;
    }

    if (op_print_instrs.specified()) {
        /* Print the disassemble code of the trace. */
        instrlist_disassemble(drir.get_drcontext(), 0, drir.get_ilist(), STDOUT);
    }
    instr_t *instr = instrlist_first(drir.get_ilist());
    uint64_t count = 0;
    while (instr != NULL) {
        count++;
        instr = instr_get_next(instr);
    }
    std::cout << "Number of Instructions: " << count << std::endl;
    std::cout << "Number of Trace Entries: " << entries.size() << std::endl;
}

/****************************************************************************
 * Options Handling
 */

static void
print_usage()
{
    std::cerr
        << CLIENT_NAME
        << ": Command-line tool that decodes the given PT raw trace and returns the "
           "outputs as specified by given flags."
        << std::endl;
    std::cerr << "Usage: " << CLIENT_NAME << " [options]" << std::endl;
    std::cerr << droption_parser_t::usage_short(DROPTION_SCOPE_FRONTEND) << std::endl;
}

static bool
option_init(int argc, const char *argv[])
{
    std::string parse_err;
    if (!droption_parser_t::parse_argv(DROPTION_SCOPE_FRONTEND, argc, argv, &parse_err,
                                       NULL)) {
        std::cerr << CLIENT_NAME << "Usage error: " << parse_err << std::endl;
        print_usage();
        return false;
    }
    if (op_help.specified()) {
        print_usage();
        return false;
    }
    if (!op_mode.specified()) {
        std::cerr << CLIENT_NAME << "Usage error: mode must be specified." << std::endl;
        print_usage();
        return false;
    }

    if (!op_raw_pt.specified() || !op_raw_pt_format.specified()) {
        std::cerr << CLIENT_NAME << "Usage error: option " << op_raw_pt.get_name()
                  << " and " << op_raw_pt_format.get_name() << "  must be specified."
                  << std::endl;
        print_usage();
        return false;
    }

    /* Because Intel PT doesn't save instruction bytes or memory contents, the converter
     * needs the instruction bytes for each IPs (Instruction Pointers) to decode the PT
     * trace.
     * drpt2trace supports two modes to convert:
     * (1) ELF Mode: the user provides an ELF file that contains all the instruction
     * bytes. So, for example, we can use this mode to convert the kernel trace and the
     * short-term user trace, where it's likely that we'll not have an image switch.
     * (2) SIDEBAND Mode: the user must provide sideband data and parameters. In this
     * mode, the converter uses sideband decoders to simulate image switches during the
     * conversion. For example, we can use this mode to convert the traces where the
     * instruction bytes are located in multiple images.
     */
    if (op_mode.get_value() == "ELF") {
        /* Check if the required options for ELF mode are specified. */
        if (!op_elf.specified()) {
            std::cerr << CLIENT_NAME << ": option " << op_elf.get_name()
                      << "is required in " << op_mode.get_value() << " mode."
                      << std::endl;
            print_usage();
            return false;
        }
    } else if (op_mode.get_value() == "SIDEBAND") {
        /* Check if the required options for SIDEBAND mode are specified. */
        if (!op_primary_sb.specified() || !op_sb_sample_type.specified()) {
            std::cerr << CLIENT_NAME << ": option " << op_primary_sb.get_name() << " and "
                      << op_sb_sample_type.get_name() << " are required in "
                      << op_mode.get_value() << " mode." << std::endl;
            print_usage();
            return false;
        }
    } else {
        std::cerr << CLIENT_NAME << ": option " << op_mode.get_name() << " is invalid."
                  << std::endl;
        print_usage();
        return false;
    }

    /* drmemtrace does not generate any sideband data. Therefore, when using this tool to
     * decode raw PT data produced by drmemtrace, the ELF mode must be employed.
     */
    if (op_raw_pt_format.get_value() == "DRMEMTRACE" && op_mode.get_value() != "ELF") {
        std::cerr << CLIENT_NAME << ": " << op_raw_pt_format.get_value()
                  << " is only supported in " << op_mode.get_value() << " mode."
                  << std::endl;
        return false;
    }

    return true;
}

/* Load binary data from file into a vector. */
static bool
load_file(IN const std::string &path, OUT std::vector<uint8_t> &buffer)
{
    /* Under C++11, there is no good solution to get the file size after using ifstream to
     * open a file. Because we will not change the PT raw trace file during converting, we
     * don't need to think about write-after-read. We get the file size from file stat
     * first and then use ifstream to open and read the PT raw trace file.
     * XXX: We may need to update Dynamorio to support C++17+. Then we can implement the
     * following logic without opening the file twice.
     */
    errno = 0;
    struct stat fstat;
    if (stat(path.c_str(), &fstat) == -1) {
        std::cerr << CLIENT_NAME << ": Failed to get file size of PT raw file: " << path
                  << ": " << errno << std::endl;
        return false;
    }
    size_t buffer_size = static_cast<size_t>(fstat.st_size);
    buffer.resize(buffer_size);

    std::ifstream f(path, std::ios::binary | std::ios::in);
    if (!f.is_open()) {
        std::cerr << CLIENT_NAME << ": Failed to open PT raw file: " << path << std::endl;
        return false;
    }
    f.read(reinterpret_cast<char *>(buffer.data()), buffer_size);
    if (f.fail()) {
        std::cerr << CLIENT_NAME << ": Failed to read PT raw file: " << path << std::endl;
        return false;
    }

    f.close();
    return true;
}

#define IF_SPECIFIED_THEN_SET(__OP_VARIABLE__, __TO_SET_VARIABLE__) \
    do {                                                            \
        if (__OP_VARIABLE__.specified()) {                          \
            __TO_SET_VARIABLE__ = __OP_VARIABLE__.get_value();      \
        }                                                           \
    } while (0)

/****************************************************************************
 * Main Function
 */

int
main(int argc, const char *argv[])
{
    /* Parse the command line. */
    if (!option_init(argc, argv)) {
        return FAILURE;
    }

    pt2ir_config_t config = {};
    config.elf_file_path = op_elf.get_value();
    config.elf_base = op_elf_base.get_value();
    config.sb_primary_file_path = op_primary_sb.get_value();
    std::istringstream op_secondary_sb_stream(op_secondary_sb.get_value());
    copy(std::istream_iterator<std::string>(op_secondary_sb_stream),
         std::istream_iterator<std::string>(),
         std::back_inserter(config.sb_secondary_file_path_list));
    config.sb_kcore_path = op_sb_kcore_path.get_value();

    /* If the user specifies the following options, drpt2trace will overwrite the
     * corresponding fields in the config.
     */
    IF_SPECIFIED_THEN_SET(op_pt_cpu_family, config.pt_config.cpu.family);
    IF_SPECIFIED_THEN_SET(op_pt_cpu_model, config.pt_config.cpu.model);
    IF_SPECIFIED_THEN_SET(op_pt_cpu_stepping, config.pt_config.cpu.stepping);
    IF_SPECIFIED_THEN_SET(op_pt_cpuid_0x15_eax, config.pt_config.cpuid_0x15_eax);
    IF_SPECIFIED_THEN_SET(op_pt_cpuid_0x15_ebx, config.pt_config.cpuid_0x15_ebx);
    IF_SPECIFIED_THEN_SET(op_pt_mtc_freq, config.pt_config.mtc_freq);
    IF_SPECIFIED_THEN_SET(op_pt_nom_freq, config.pt_config.nom_freq);
    IF_SPECIFIED_THEN_SET(op_sb_sample_type, config.sb_config.sample_type);
    IF_SPECIFIED_THEN_SET(op_sb_sysroot, config.sb_config.sysroot);
    IF_SPECIFIED_THEN_SET(op_sb_time_zero, config.sb_config.time_zero);
    IF_SPECIFIED_THEN_SET(op_sb_time_shift, config.sb_config.time_shift);
    IF_SPECIFIED_THEN_SET(op_sb_time_mult, config.sb_config.time_mult);
    IF_SPECIFIED_THEN_SET(op_sb_tsc_offset, config.sb_config.tsc_offset);
    IF_SPECIFIED_THEN_SET(op_sb_kernel_start, config.sb_config.kernel_start);
    config.pt_config.cpu.vendor =
        config.pt_config.cpu.family != 0 ? CPU_VENDOR_INTEL : CPU_VENDOR_UNKNOWN;

    /* Convert the PT raw trace to DR IR. */
    std::unique_ptr<pt_iscache_autoclean_t> shared_iscache(
        new pt_iscache_autoclean_t(pt_iscache_alloc(nullptr)));
    std::unique_ptr<pt2ir_t> ptconverter(new pt2ir_t());
    drir_t drir(GLOBAL_DCONTEXT);

    /* Read the PT data from PT raw trace file. */
    std::vector<uint8_t> pt_raw_buffer;
    if (!load_file(op_raw_pt.get_value(), pt_raw_buffer)) {
        std::cerr << CLIENT_NAME << ": failed to read PT raw trace." << std::endl;
        return FAILURE;
    }

    if (op_raw_pt_format.get_value() == "PERF") {
        config.pt_raw_buffer_size = pt_raw_buffer.size();
        if (!ptconverter->init(config, shared_iscache.get()->iscache)) {
            std::cerr << CLIENT_NAME << ": failed to initialize pt2ir_t." << std::endl;
            return FAILURE;
        }

        uint8_t *pt_data = pt_raw_buffer.data();
        size_t pt_data_size = pt_raw_buffer.size();
        pt2ir_convert_status_t status = ptconverter->convert(pt_data, pt_data_size, drir);
        if (status != PT2IR_CONV_SUCCESS) {
            std::cerr << CLIENT_NAME << ": failed to convert PT raw trace to DR IR."
                      << "[error status: " << status << "]" << std::endl;
            return FAILURE;
        }
    } else if (op_raw_pt_format.get_value() == "DRMEMTRACE") {
        syscall_pt_entry_t *pt_metadata_header =
            reinterpret_cast<syscall_pt_entry_t *>(pt_raw_buffer.data());

        if (pt_metadata_header[PDB_HEADER_DATA_BOUNDARY_IDX].pt_metadata_boundary.type !=
            SYSCALL_PT_ENTRY_TYPE_PT_METADATA_BOUNDARY) {
            std::cerr << CLIENT_NAME << ": invalid PT raw trace format." << std::endl;
            return FAILURE;
        }

        void *metadata_buffer =
            reinterpret_cast<void *>(reinterpret_cast<uint8_t *>(pt_metadata_header) +
                                     PT_METADATA_PDB_DATA_OFFSET);
        config.init_with_metadata(metadata_buffer);

        /* Set the buffer size to be at least the maximum stream data size.
         */
#define RING_BUFFER_SIZE_SHIFT 8
        config.pt_raw_buffer_size =
            (1L << RING_BUFFER_SIZE_SHIFT) * sysconf(_SC_PAGESIZE);

        /* Initialize the ptconverter. */
        if (!ptconverter->init(config, shared_iscache.get()->iscache)) {
            std::cerr << CLIENT_NAME << ": failed to initialize pt2ir_t." << std::endl;
            return FAILURE;
        }

        size_t cur_pdb_header_offset = PT_METADATA_PDB_HEADER_SIZE +
            pt_metadata_header[PDB_HEADER_DATA_BOUNDARY_IDX]
                .pt_metadata_boundary.data_size;
        while (cur_pdb_header_offset < pt_raw_buffer.size()) {
            if (cur_pdb_header_offset + PT_DATA_PDB_HEADER_SIZE > pt_raw_buffer.size()) {
                std::cerr << CLIENT_NAME << ": invalid PT raw trace format." << std::endl;
                return FAILURE;
            }

            /* Read the PT Data Buffer's header and get the PT Data. */
            syscall_pt_entry_t *cur_pdb_header = reinterpret_cast<syscall_pt_entry_t *>(
                pt_raw_buffer.data() + cur_pdb_header_offset);
            if (cur_pdb_header[PDB_HEADER_DATA_BOUNDARY_IDX].pt_data_boundary.type !=
                SYSCALL_PT_ENTRY_TYPE_PT_DATA_BOUNDARY) {
                std::cerr << CLIENT_NAME << ": invalid PT raw trace format." << std::endl;
                return FAILURE;
            }
            uint64_t syscall_args_num =
                cur_pdb_header[PDB_HEADER_NUM_ARGS_IDX].syscall_args_num.args_num;
            uint64_t pt_data_size =
                cur_pdb_header[PDB_HEADER_DATA_BOUNDARY_IDX].pt_data_boundary.data_size -
                SYSCALL_METADATA_SIZE - syscall_args_num * sizeof(uint64_t);
            uint8_t *pt_data = reinterpret_cast<uint8_t *>(cur_pdb_header) +
                PT_DATA_PDB_DATA_OFFSET + syscall_args_num * sizeof(uint64_t);

            /* Convert the PT Data to DR IR. */
            pt2ir_convert_status_t status =
                ptconverter->convert(pt_data, pt_data_size, drir);
            if (status != PT2IR_CONV_SUCCESS) {
                std::cerr << CLIENT_NAME << ": failed to convert PT raw trace to DR IR."
                          << "[error status: " << status << "]" << std::endl;
                return FAILURE;
            }

            /* Update the offset to the next PT Data Buffer's header. */
            cur_pdb_header_offset += PT_DATA_PDB_HEADER_SIZE +
                syscall_args_num * sizeof(uint64_t) + pt_data_size;
        }
    } else {
        std::cerr << CLIENT_NAME
                  << ": unknown raw PT format: " << op_raw_pt_format.get_value()
                  << std::endl;
        return FAILURE;
    }

    /* Convert the DR IR to trace entries. */
    std::vector<trace_entry_t> entries;
    ir2trace_convert_status_t ir2trace_convert_status =
        ir2trace_t::convert(drir, entries);
    if (ir2trace_convert_status != IR2TRACE_CONV_SUCCESS) {
        std::cerr << CLIENT_NAME << ": failed to convert DR IR to trace entries."
                  << "[error status: " << ir2trace_convert_status << "]" << std::endl;
        return FAILURE;
    }

    /* Print the disassemble code of instructions and the trace entries count. */
    print_results(drir, entries);

    return SUCCESS;
}
