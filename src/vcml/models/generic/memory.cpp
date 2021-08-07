/******************************************************************************
 *                                                                            *
 * Copyright 2018 Jan Henrik Weinstock                                        *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License");            *
 * you may not use this file except in compliance with the License.           *
 * You may obtain a copy of the License at                                    *
 *                                                                            *
 *     http://www.apache.org/licenses/LICENSE-2.0                             *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,          *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 *                                                                            *
 ******************************************************************************/

#include <sys/mman.h>

#include "vcml/models/generic/memory.h"

namespace vcml { namespace generic {

    bool memory::cmd_show(const vector<string>& args, ostream& os) {
        u64 start = strtoull(args[0].c_str(), NULL, 0);
        u64 end = strtoull(args[1].c_str(), NULL, 0);

        if ((end <= start) || (end >= size))
            return false;

        #define HEX(x, w) std::setfill('0') << std::setw(w) << \
                          std::hex << x << std::dec
        os << "showing range 0x" << HEX(start, 8) << " .. 0x" << HEX(end, 8);

        u64 addr = start & ~0xf;
        while (addr < end) {
            if ((addr % 16) == 0)
                os << "\n" << HEX(addr, 8) << ":";
            if ((addr % 4) == 0)
                os << " ";
            if (addr >= start)
                os << HEX((unsigned int)m_memory[addr], 2) << " ";
            else
                os << "   ";
            addr++;
        }

        #undef HEX
        return true;
    }

    u8* memory::allocate_image(u64 sz, u64 off) {
        if (off >= size)
            VCML_REPORT("offset 0x%lx exceeds memory size", off);

        if (sz + off > size)
            VCML_REPORT("image too big for memory");

        return m_memory + off;
    }

    void memory::copy_image(const u8* image, u64 sz, u64 off) {
        if (off >= size)
            VCML_REPORT("offset 0x%lx exceeds memory size", off);

        if (sz + off > size)
            VCML_REPORT("image too big for memory");

        memcpy(m_memory + off, image, sz);
    }

    memory::memory(const sc_module_name& nm, u64 sz, bool read_only,
                   unsigned int alignment, unsigned int rl, unsigned int wl):
        peripheral(nm, host_endian(), rl, wl),
        debugging::loader(name()),
        m_base(nullptr),
        m_memory(nullptr),
        size("size", sz),
        align("align", alignment),
        discard_writes("discard_writes", false),
        readonly("readonly", read_only),
        images("images", ""),
        poison("poison", 0x00),
        IN("IN") {
        VCML_ERROR_ON(size == 0u, "memory size cannot be 0");
        VCML_ERROR_ON(align >= 64u, "requested alignment too big");

        const int perms = PROT_READ | PROT_WRITE;
        const int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;

        std::uintptr_t extra = (1ull << align) - 1;
        m_base = mmap(0, size + extra, perms, flags, -1, 0);
        VCML_ERROR_ON(m_base == MAP_FAILED, "mmap failed: %s", strerror(errno));
        m_memory = (u8*)(((std::uintptr_t)m_base + extra) & ~extra);

        map_dmi(m_memory, 0, size - 1, readonly ? VCML_ACCESS_READ
                                                : VCML_ACCESS_READ_WRITE);

        register_command("show", 2, this, &memory::cmd_show,
            "show memory contents between addresses [start] and [end]. "
            "usage: show [start] [end]");
    }

    memory::~memory() {
        if (m_base)
            munmap(m_base, size);
    }

    void memory::reset() {
        if (poison > 0)
            memset(m_memory, poison, size);

        load_images(images);
    }

    tlm_response_status memory::read(const range& addr, void* data,
                                     const tlm_sbi& info) {
        if (addr.end >= size)
            return TLM_ADDRESS_ERROR_RESPONSE;
        memcpy(data, m_memory + addr.start, addr.length());
        return TLM_OK_RESPONSE;
    }

    tlm_response_status memory::write(const range& addr, const void* data,
                                      const tlm_sbi& info) {
        if (addr.end >= size)
            return TLM_ADDRESS_ERROR_RESPONSE;
        if (!info.is_debug && discard_writes)
            return TLM_OK_RESPONSE;
        if (!info.is_debug && readonly)
            return TLM_COMMAND_ERROR_RESPONSE;
        memcpy(m_memory + addr.start, data, addr.length());
        return TLM_OK_RESPONSE;
    }

}}
