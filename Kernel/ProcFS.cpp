#include "ProcFS.h"
#include "Process.h"
#include <Kernel/VirtualFileSystem.h>
#include "system.h"
#include "MemoryManager.h"
#include "StdLib.h"
#include "i386.h"
#include "KSyms.h"
#include "Console.h"
#include "Scheduler.h"
#include <AK/StringBuilder.h>
#include <LibC/errno_numbers.h>

enum ProcParentDirectory {
    PDI_AbstractRoot = 0,
    PDI_Root,
    PDI_Root_sys,
    PDI_PID,
    PDI_PID_fd,
};

enum ProcFileType {
    FI_Invalid = 0,

    FI_Root = 1, // directory

    __FI_Root_Start,
    FI_Root_mm,
    FI_Root_mounts,
    FI_Root_kmalloc,
    FI_Root_all,
    FI_Root_summary,
    FI_Root_cpuinfo,
    FI_Root_inodes,
    FI_Root_dmesg,
    FI_Root_self, // symlink
    FI_Root_sys, // directory
    __FI_Root_End,

    FI_PID,

    __FI_PID_Start,
    FI_PID_vm,
    FI_PID_vmo,
    FI_PID_stack,
    FI_PID_regs,
    FI_PID_fds,
    FI_PID_exe, // symlink
    FI_PID_cwd, // symlink
    FI_PID_fd, // directory
    __FI_PID_End,

    FI_MaxStaticFileIndex,
};

static inline pid_t to_pid(const InodeIdentifier& identifier)
{
#ifdef PROCFS_DEBUG
    dbgprintf("to_pid, index=%08x -> %u\n", identifier.index(), identifier.index() >> 16);
#endif
    return identifier.index() >> 16u;
}

static inline ProcParentDirectory to_proc_parent_directory(const InodeIdentifier& identifier)
{
    return (ProcParentDirectory)((identifier.index() >> 12) & 0xf);
}

static inline int to_fd(const InodeIdentifier& identifier)
{
    ASSERT(to_proc_parent_directory(identifier) == PDI_PID_fd);
    return (identifier.index() & 0xff) - FI_MaxStaticFileIndex;
}

static inline unsigned to_sys_index(const InodeIdentifier& identifier)
{
    ASSERT(to_proc_parent_directory(identifier) == PDI_Root_sys);
    return identifier.index() & 0xff;
}

static inline InodeIdentifier to_identifier(unsigned fsid, ProcParentDirectory parent, pid_t pid, ProcFileType proc_file_type)
{
    return { fsid, ((unsigned)parent << 12u) | ((unsigned)pid << 16u) | (unsigned)proc_file_type };
}

static inline InodeIdentifier to_identifier_with_fd(unsigned fsid, pid_t pid, int fd)
{
    return { fsid, (PDI_PID_fd << 12u) | ((unsigned)pid << 16u) | (FI_MaxStaticFileIndex + fd) };
}

static inline InodeIdentifier sys_var_to_identifier(unsigned fsid, unsigned index)
{
    ASSERT(index < 256);
    return { fsid, (PDI_Root_sys << 12u) | index };
}

static inline InodeIdentifier to_parent_id(const InodeIdentifier& identifier)
{
    switch (to_proc_parent_directory(identifier)) {
    case PDI_AbstractRoot:
    case PDI_Root:
        return { identifier.fsid(), FI_Root };
    case PDI_Root_sys:
        return { identifier.fsid(), FI_Root_sys };
    case PDI_PID:
        return to_identifier(identifier.fsid(), PDI_Root, to_pid(identifier), FI_PID);
    case PDI_PID_fd:
        return to_identifier(identifier.fsid(), PDI_PID, to_pid(identifier), FI_PID_fd);
    }
}

#if 0
static inline byte to_unused_metadata(const InodeIdentifier& identifier)
{
    return (identifier.index() >> 8) & 0xf;
}
#endif

static inline ProcFileType to_proc_file_type(const InodeIdentifier& identifier)
{
    return (ProcFileType)(identifier.index() & 0xff);
}

static inline bool is_process_related_file(const InodeIdentifier& identifier)
{
    if (to_proc_file_type(identifier) == FI_PID)
        return true;
    auto proc_parent_directory = to_proc_parent_directory(identifier);
    switch (proc_parent_directory) {
    case PDI_PID:
    case PDI_PID_fd:
        return true;
    default:
        return false;
    }
}

static inline bool is_directory(const InodeIdentifier& identifier)
{
    auto proc_file_type = to_proc_file_type(identifier);
    switch (proc_file_type) {
    case FI_Root:
    case FI_Root_sys:
    case FI_PID:
    case FI_PID_fd:
        return true;
    default:
        return false;
    }
}

static inline bool is_persistent_inode(const InodeIdentifier& identifier)
{
    return to_proc_parent_directory(identifier) == PDI_Root_sys;
}

static ProcFS* s_the;

ProcFS& ProcFS::the()
{
    ASSERT(s_the);
    return *s_the;
}

RetainPtr<ProcFS> ProcFS::create()
{
    return adopt(*new ProcFS);
}

ProcFS::~ProcFS()
{
}

ByteBuffer procfs$pid_fds(InodeIdentifier identifier)
{
    auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier));
    if (!handle)
        return { };
    auto& process = handle->process();
    if (process.number_of_open_file_descriptors() == 0)
        return { };
    StringBuilder builder;
    for (size_t i = 0; i < process.max_open_file_descriptors(); ++i) {
        auto* descriptor = process.file_descriptor(i);
        if (!descriptor)
            continue;
        builder.appendf("% 3u %s\n", i, descriptor->absolute_path().characters());
    }
    return builder.to_byte_buffer();
}

ByteBuffer procfs$pid_fd_entry(InodeIdentifier identifier)
{
    auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier));
    if (!handle)
        return { };
    auto& process = handle->process();
    int fd = to_fd(identifier);
    auto* descriptor = process.file_descriptor(fd);
    if (!descriptor)
        return { };
    return descriptor->absolute_path().to_byte_buffer();
}

ByteBuffer procfs$pid_vm(InodeIdentifier identifier)
{
#ifdef PROCFS_DEBUG
    dbgprintf("pid_vm: pid=%d\n", to_pid(identifier));
#endif
    auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier));
    if (!handle)
        return { };
    auto& process = handle->process();
    StringBuilder builder;
    builder.appendf("BEGIN       END         SIZE      COMMIT     NAME\n");
    for (auto& region : process.regions()) {
        builder.appendf("%x -- %x    %x  %x   %s\n",
            region->laddr().get(),
            region->laddr().offset(region->size() - 1).get(),
            region->size(),
            region->amount_resident(),
            region->name().characters());
    }
    return builder.to_byte_buffer();
}

ByteBuffer procfs$pid_vmo(InodeIdentifier identifier)
{
    auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier));
    if (!handle)
        return { };
    auto& process = handle->process();
    StringBuilder builder;
    builder.appendf("BEGIN       END         SIZE        NAME\n");
    for (auto& region : process.regions()) {
        builder.appendf("%x -- %x    %x    %s\n",
            region->laddr().get(),
            region->laddr().offset(region->size() - 1).get(),
            region->size(),
            region->name().characters());
        builder.appendf("VMO: %s \"%s\" @ %x(%u)\n",
            region->vmo().is_anonymous() ? "anonymous" : "file-backed",
            region->vmo().name().characters(),
            &region->vmo(),
            region->vmo().retain_count());
        for (size_t i = 0; i < region->vmo().page_count(); ++i) {
            auto& physical_page = region->vmo().physical_pages()[i];
            builder.appendf("P%x%s(%u) ",
                physical_page ? physical_page->paddr().get() : 0,
                region->cow_map().get(i) ? "!" : "",
                physical_page ? physical_page->retain_count() : 0
            );
        }
        builder.appendf("\n");
    }
    return builder.to_byte_buffer();
}

ByteBuffer procfs$pid_stack(InodeIdentifier identifier)
{
    auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier));
    if (!handle)
        return { };
    auto& process = handle->process();
    ProcessPagingScope paging_scope(process);
    struct RecognizedSymbol {
        dword address;
        const KSym* ksym;
    };
    Vector<RecognizedSymbol> recognized_symbols;
    if (auto* eip_ksym = ksymbolicate(process.tss().eip))
        recognized_symbols.append({ process.tss().eip, eip_ksym });
    for (dword* stack_ptr = (dword*)process.frame_ptr(); process.validate_read_from_kernel(LinearAddress((dword)stack_ptr)); stack_ptr = (dword*)*stack_ptr) {
        dword retaddr = stack_ptr[1];
        if (auto* ksym = ksymbolicate(retaddr))
            recognized_symbols.append({ retaddr, ksym });
    }
    StringBuilder builder;
    for (auto& symbol : recognized_symbols) {
        unsigned offset = symbol.address - symbol.ksym->address;
        builder.appendf("%p  %s +%u\n", symbol.address, symbol.ksym->name, offset);
    }
    return builder.to_byte_buffer();
}

ByteBuffer procfs$pid_regs(InodeIdentifier identifier)
{
    auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier));
    if (!handle)
        return { };
    auto& process = handle->process();
    auto& tss = process.tss();
    StringBuilder builder;
    builder.appendf("eax: %x\n", tss.eax);
    builder.appendf("ebx: %x\n", tss.ebx);
    builder.appendf("ecx: %x\n", tss.ecx);
    builder.appendf("edx: %x\n", tss.edx);
    builder.appendf("esi: %x\n", tss.esi);
    builder.appendf("edi: %x\n", tss.edi);
    builder.appendf("ebp: %x\n", tss.ebp);
    builder.appendf("cr3: %x\n", tss.cr3);
    builder.appendf("flg: %x\n", tss.eflags);
    builder.appendf("sp:  %w:%x\n", tss.ss, tss.esp);
    builder.appendf("pc:  %w:%x\n", tss.cs, tss.eip);
    return builder.to_byte_buffer();
}

ByteBuffer procfs$pid_exe(InodeIdentifier identifier)
{
    auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier));
    if (!handle)
        return { };
    auto& process = handle->process();
    auto inode = process.executable_inode();
    ASSERT(inode);
    return VFS::the().absolute_path(*inode).to_byte_buffer();
}

ByteBuffer procfs$pid_cwd(InodeIdentifier identifier)
{
    auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier));
    if (!handle)
        return { };
    auto& process = handle->process();
    auto inode = process.cwd_inode();
    ASSERT(inode);
    return VFS::the().absolute_path(*inode).to_byte_buffer();
}

ByteBuffer procfs$self(InodeIdentifier)
{
    char buffer[16];
    ksprintf(buffer, "%u", current->pid());
    return ByteBuffer::copy((const byte*)buffer, strlen(buffer));
}

ByteBuffer procfs$mm(InodeIdentifier)
{
    // FIXME: Implement
    InterruptDisabler disabler;
    StringBuilder builder;
    for (auto* vmo : MM.m_vmos) {
        builder.appendf("VMO: %p %s(%u): p:%4u %s\n",
            vmo,
            vmo->is_anonymous() ? "anon" : "file",
            vmo->retain_count(),
            vmo->page_count(),
            vmo->name().characters());
    }
    builder.appendf("VMO count: %u\n", MM.m_vmos.size());
    builder.appendf("Free physical pages: %u\n", MM.m_free_physical_pages.size());
    builder.appendf("Free supervisor physical pages: %u\n", MM.m_free_supervisor_physical_pages.size());
    return builder.to_byte_buffer();
}

ByteBuffer procfs$dmesg(InodeIdentifier)
{
    InterruptDisabler disabler;
    StringBuilder builder;
    for (char ch : Console::the().logbuffer())
        builder.append(ch);
    return builder.to_byte_buffer();
}

ByteBuffer procfs$mounts(InodeIdentifier)
{
    // FIXME: This is obviously racy against the VFS mounts changing.
    StringBuilder builder;
    VFS::the().for_each_mount([&builder] (auto& mount) {
        auto& fs = mount.guest_fs();
        builder.appendf("%s @ ", fs.class_name());
        if (!mount.host().is_valid())
            builder.appendf("/");
        else {
            builder.appendf("%u:%u", mount.host().fsid(), mount.host().index());
            auto path = VFS::the().absolute_path(mount.host());
            builder.append(' ');
            builder.append(path);
        }
        builder.append('\n');
    });
    return builder.to_byte_buffer();
}

ByteBuffer procfs$cpuinfo(InodeIdentifier)
{
    StringBuilder builder;
    {
        CPUID cpuid(0);
        builder.appendf("cpuid:     ");
        auto emit_dword = [&] (dword value) {
            builder.appendf("%c%c%c%c",
                value & 0xff,
                (value >> 8) & 0xff,
                (value >> 16) & 0xff,
                (value >> 24) & 0xff);
        };
        emit_dword(cpuid.ebx());
        emit_dword(cpuid.edx());
        emit_dword(cpuid.ecx());
        builder.appendf("\n");
    }
    {
        CPUID cpuid(1);
        dword stepping = cpuid.eax() & 0xf;
        dword model = (cpuid.eax() >> 4) & 0xf;
        dword family = (cpuid.eax() >> 8) & 0xf;
        dword type = (cpuid.eax() >> 12) & 0x3;
        dword extended_model = (cpuid.eax() >> 16) & 0xf;
        dword extended_family = (cpuid.eax() >> 20) & 0xff;
        dword display_model;
        dword display_family;
        if (family == 15) {
            display_family = family + extended_family;
            display_model = model + (extended_model << 4);
        } else if (family == 6) {
            display_family = family;
            display_model = model + (extended_model << 4);
        } else {
            display_family = family;
            display_model = model;
        }
        builder.appendf("family:    %u\n", display_family);
        builder.appendf("model:     %u\n", display_model);
        builder.appendf("stepping:  %u\n", stepping);
        builder.appendf("type:      %u\n", type);
    }
    {
        // FIXME: Check first that this is supported by calling CPUID with eax=0x80000000
        //        and verifying that the returned eax>=0x80000004.
        char buffer[48];
        dword* bufptr = reinterpret_cast<dword*>(buffer);
        auto copy_brand_string_part_to_buffer = [&] (dword i) {
            CPUID cpuid(0x80000002 + i);
            *bufptr++ = cpuid.eax();
            *bufptr++ = cpuid.ebx();
            *bufptr++ = cpuid.ecx();
            *bufptr++ = cpuid.edx();
        };
        copy_brand_string_part_to_buffer(0);
        copy_brand_string_part_to_buffer(1);
        copy_brand_string_part_to_buffer(2);
        builder.appendf("brandstr:  \"%s\"\n", buffer);
    }
    return builder.to_byte_buffer();
}

ByteBuffer procfs$kmalloc(InodeIdentifier)
{
    StringBuilder builder;
    builder.appendf(
        "eternal:      %u\n"
        "allocated:    %u\n"
        "free:         %u\n",
        kmalloc_sum_eternal,
        sum_alloc,
        sum_free
    );
    return builder.to_byte_buffer();
}

ByteBuffer procfs$summary(InodeIdentifier)
{
    InterruptDisabler disabler;
    auto processes = Process::all_processes();
    StringBuilder builder;
    builder.appendf("PID TPG PGP SID  OWNER  STATE      PPID NSCHED     FDS  TTY  NAME\n");
    for (auto* process : processes) {
        builder.appendf("% 3u % 3u % 3u % 3u  % 4u   % 8s   % 3u  % 9u  % 3u  % 4s  %s\n",
            process->pid(),
            process->tty() ? process->tty()->pgid() : 0,
            process->pgid(),
            process->sid(),
            process->uid(),
            to_string(process->state()),
            process->ppid(),
            process->times_scheduled(),
            process->number_of_open_file_descriptors(),
            process->tty() ? strrchr(process->tty()->tty_name().characters(), '/') + 1 : "n/a",
            process->name().characters());
    }
    return builder.to_byte_buffer();
}

ByteBuffer procfs$all(InodeIdentifier)
{
    InterruptDisabler disabler;
    auto processes = Process::all_processes();
    StringBuilder builder;
    auto build_process_line = [&builder] (Process* process) {
        builder.appendf("%u,%u,%u,%u,%u,%u,%u,%s,%u,%u,%s,%s,%u,%u,%u\n",
            process->pid(),
            process->times_scheduled(),
            process->tty() ? process->tty()->pgid() : 0,
            process->pgid(),
            process->sid(),
            process->uid(),
            process->gid(),
            to_string(process->state()),
            process->ppid(),
            process->number_of_open_file_descriptors(),
            process->tty() ? process->tty()->tty_name().characters() : "notty",
            process->name().characters(),
            process->amount_virtual(),
            process->amount_resident(),
            process->amount_shared()
        );
    };
    build_process_line(Scheduler::colonel());
    for (auto* process : processes)
        build_process_line(process);
    return builder.to_byte_buffer();
}

ByteBuffer procfs$inodes(InodeIdentifier)
{
    extern HashTable<Inode*>& all_inodes();
    auto& vfs = VFS::the();
    StringBuilder builder;
    for (auto it : all_inodes()) {
        RetainPtr<Inode> inode = *it;
        String path = vfs.absolute_path(*inode);
        builder.appendf("Inode{K%x} %02u:%08u (%u) %s\n", inode.ptr(), inode->fsid(), inode->index(), inode->retain_count(), path.characters());
    }
    return builder.to_byte_buffer();
}

struct SysVariableData final : public ProcFSInodeCustomData {
    virtual ~SysVariableData() override { }

    enum Type {
        Invalid,
        Boolean,
    };
    Type type { Invalid };
    Function<void()> notify_callback;
    void* address;
};

static ByteBuffer read_sys_bool(InodeIdentifier inode_id)
{
    auto inode_ptr = ProcFS::the().get_inode(inode_id);
    if (!inode_ptr)
        return { };
    auto& inode = static_cast<ProcFSInode&>(*inode_ptr);
    ASSERT(inode.custom_data());
    auto buffer = ByteBuffer::create_uninitialized(2);
    auto& custom_data = *static_cast<const SysVariableData*>(inode.custom_data());
    ASSERT(custom_data.type == SysVariableData::Boolean);
    ASSERT(custom_data.address);
    buffer[0] = *reinterpret_cast<bool*>(custom_data.address) ? '1' : '0';
    buffer[1] = '\n';
    return buffer;
}

static ssize_t write_sys_bool(InodeIdentifier inode_id, const ByteBuffer& data)
{
    auto inode_ptr = ProcFS::the().get_inode(inode_id);
    if (!inode_ptr)
        return { };
    auto& inode = static_cast<ProcFSInode&>(*inode_ptr);
    ASSERT(inode.custom_data());
    if (data.size() >= 1 && (data[0] == '0' || data[0] == '1')) {
        auto& custom_data = *static_cast<const SysVariableData*>(inode.custom_data());
        ASSERT(custom_data.address);
        bool new_value = data[0] == '1';
        *reinterpret_cast<bool*>(custom_data.address) = new_value;
        if (custom_data.notify_callback)
            custom_data.notify_callback();
    }
    return data.size();
}

void ProcFS::add_sys_bool(String&& name, bool* var, Function<void()>&& notify_callback)
{
    InterruptDisabler disabler;

    unsigned index = m_sys_entries.size();
    auto inode = adopt(*new ProcFSInode(*this, sys_var_to_identifier(fsid(), index).index()));
    auto data = make<SysVariableData>();
    data->type = SysVariableData::Boolean;
    data->notify_callback = move(notify_callback);
    data->address = var;
    inode->set_custom_data(move(data));
    m_sys_entries.append({ strdup(name.characters()), name.length(), read_sys_bool, write_sys_bool, move(inode) });
}

bool ProcFS::initialize()
{
    return true;
}

const char* ProcFS::class_name() const
{
    return "ProcFS";
}

RetainPtr<Inode> ProcFS::create_inode(InodeIdentifier parentInode, const String& name, mode_t mode, unsigned size, int& error)
{
    (void) parentInode;
    (void) name;
    (void) mode;
    (void) size;
    (void) error;
    kprintf("FIXME: Implement ProcFS::create_inode()?\n");
    return { };
}

RetainPtr<Inode> ProcFS::create_directory(InodeIdentifier, const String&, mode_t, int& error)
{
    error = -EROFS;
    return nullptr;
}

RetainPtr<Inode> ProcFSInode::parent() const
{
    return fs().get_inode(to_parent_id(identifier()));
}

InodeIdentifier ProcFS::root_inode() const
{
    return { fsid(), FI_Root };
}

RetainPtr<Inode> ProcFS::get_inode(InodeIdentifier inode_id) const
{
#ifdef PROCFS_DEBUG
    dbgprintf("ProcFS::get_inode(%u)\n", inode_id.index());
#endif
    if (inode_id == root_inode())
        return m_root_inode;

    if (to_proc_parent_directory(inode_id) == ProcParentDirectory::PDI_Root_sys) {
        auto sys_index = to_sys_index(inode_id);
        if (sys_index < m_sys_entries.size())
            return m_sys_entries[sys_index].inode;
    }

    LOCKER(m_inodes_lock);
    auto it = m_inodes.find(inode_id.index());
    if (it == m_inodes.end()) {
        auto inode = adopt(*new ProcFSInode(const_cast<ProcFS&>(*this), inode_id.index()));
        m_inodes.set(inode_id.index(), inode.ptr());
        return inode;
    }
    return (*it).value;
}

ProcFSInode::ProcFSInode(ProcFS& fs, unsigned index)
    : Inode(fs, index)
{
}

ProcFSInode::~ProcFSInode()
{
    LOCKER(fs().m_inodes_lock);
    fs().m_inodes.remove(index());
}

InodeMetadata ProcFSInode::metadata() const
{
#ifdef PROCFS_DEBUG
    dbgprintf("ProcFSInode::metadata(%u)\n", index());
#endif
    InodeMetadata metadata;
    metadata.inode = identifier();
    metadata.ctime = mepoch;
    metadata.atime = mepoch;
    metadata.mtime = mepoch;
    auto proc_parent_directory = to_proc_parent_directory(identifier());
    auto pid = to_pid(identifier());
    auto proc_file_type = to_proc_file_type(identifier());

#ifdef PROCFS_DEBUG
    dbgprintf("  -> pid: %d, fi: %u, pdi: %u\n", pid, proc_file_type, proc_parent_directory);
#endif

    if (is_process_related_file(identifier())) {
        auto handle = ProcessInspectionHandle::from_pid(pid);
        metadata.uid = handle->process().sys$getuid();
        metadata.gid = handle->process().sys$getgid();
    }

    if (proc_parent_directory == PDI_PID_fd) {
        metadata.mode = 00120777;
        return metadata;
    }

    switch (proc_file_type) {
    case FI_Root_self:
    case FI_PID_cwd:
    case FI_PID_exe:
        metadata.mode = 0120777;
        break;
    case FI_Root:
    case FI_Root_sys:
    case FI_PID:
    case FI_PID_fd:
        metadata.mode = 040777;
        break;
    default:
        metadata.mode = 0100644;
        break;
    }
#ifdef PROCFS_DEBUG
    dbgprintf("Returning mode %o\n", metadata.mode);
#endif
    return metadata;
}

ssize_t ProcFSInode::read_bytes(off_t offset, size_t count, byte* buffer, FileDescriptor* descriptor) const
{
#ifdef PROCFS_DEBUG
    dbgprintf("ProcFS: read_bytes %u\n", index());
#endif
    ASSERT(offset >= 0);
    ASSERT(buffer);

    auto* directory_entry = fs().get_directory_entry(identifier());

    Function<ByteBuffer(InodeIdentifier)> callback_tmp;
    Function<ByteBuffer(InodeIdentifier)>* read_callback { nullptr };
    if (directory_entry) {
        read_callback = &directory_entry->read_callback;
    } else {
        if (to_proc_parent_directory(identifier()) == PDI_PID_fd) {
            callback_tmp = procfs$pid_fd_entry;
            read_callback = &callback_tmp;
        }
    }

    ASSERT(read_callback);

    ByteBuffer generated_data;
    if (!descriptor) {
        generated_data = (*read_callback)(identifier());
    } else {
        if (!descriptor->generator_cache())
            descriptor->generator_cache() = (*read_callback)(identifier());
        generated_data = descriptor->generator_cache();
    }

    auto& data = generated_data;
    ssize_t nread = min(static_cast<off_t>(data.size() - offset), static_cast<off_t>(count));
    memcpy(buffer, data.pointer() + offset, nread);
    if (nread == 0 && descriptor && descriptor->generator_cache())
        descriptor->generator_cache().clear();
    return nread;
}

InodeIdentifier ProcFS::ProcFSDirectoryEntry::identifier(unsigned fsid) const
{
    return to_identifier(fsid, PDI_Root, 0, (ProcFileType)proc_file_type);
}

bool ProcFSInode::traverse_as_directory(Function<bool(const FS::DirectoryEntry&)> callback) const
{
#ifdef PROCFS_DEBUG
    dbgprintf("ProcFS: traverse_as_directory %u\n", index());
#endif

    if (!::is_directory(identifier()))
        return false;

    auto pid = to_pid(identifier());
    auto proc_file_type = to_proc_file_type(identifier());
    auto parent_id = to_parent_id(identifier());

    callback({ ".", 1, identifier(), 2 });
    callback({ "..", 2, parent_id, 2 });

    switch (proc_file_type) {
    case FI_Root:
        for (auto& entry : fs().m_entries) {
            // FIXME: strlen() here is sad.
            if (!entry.name)
                continue;
            if (entry.proc_file_type > __FI_Root_Start && entry.proc_file_type < __FI_Root_End)
                callback({ entry.name, strlen(entry.name), to_identifier(fsid(), PDI_Root, 0, (ProcFileType)entry.proc_file_type), 0 });
        }
        for (auto pid_child : Process::all_pids()) {
            char name[16];
            size_t name_length = ksprintf(name, "%u", pid_child);
            callback({ name, name_length, to_identifier(fsid(), PDI_Root, pid_child, FI_PID), 0 });
        }
        break;

    case FI_Root_sys:
        for (size_t i = 0; i < fs().m_sys_entries.size(); ++i) {
            auto& entry = fs().m_sys_entries[i];
            callback({ entry.name, strlen(entry.name), sys_var_to_identifier(fsid(), i), 0 });
        }
        break;

    case FI_PID: {
        auto handle = ProcessInspectionHandle::from_pid(pid);
        if (!handle)
            return false;
        auto& process = handle->process();
        for (auto& entry : fs().m_entries) {
            if (entry.proc_file_type > __FI_PID_Start && entry.proc_file_type < __FI_PID_End) {
                if (entry.proc_file_type == FI_PID_exe || entry.proc_file_type == FI_PID_cwd) {
                    if (entry.proc_file_type == FI_PID_exe && !process.executable_inode())
                        continue;
                    if (entry.proc_file_type == FI_PID_cwd && !process.cwd_inode())
                        continue;
                }
                // FIXME: strlen() here is sad.
                callback({ entry.name, strlen(entry.name), to_identifier(fsid(), PDI_PID, pid, (ProcFileType)entry.proc_file_type), 0 });
            }
        }
        }
        break;

    case FI_PID_fd: {
        auto handle = ProcessInspectionHandle::from_pid(pid);
        if (!handle)
            return false;
        auto& process = handle->process();
        for (size_t i = 0; i < process.max_open_file_descriptors(); ++i) {
            auto* descriptor = process.file_descriptor(i);
            if (!descriptor)
                continue;
            char name[16];
            size_t name_length = ksprintf(name, "%u", i);
            callback({ name, name_length, to_identifier_with_fd(fsid(), pid, i), 0 });
        }
        }
        break;
    default:
        return true;
    }

    return true;
}

InodeIdentifier ProcFSInode::lookup(const String& name)
{
    ASSERT(is_directory());
    if (name == ".")
        return identifier();
    if (name == "..")
        return to_parent_id(identifier());

    auto proc_file_type = to_proc_file_type(identifier());

    if (proc_file_type == FI_Root) {
        for (auto& entry : fs().m_entries) {
            if (entry.name == nullptr)
                continue;
            if (entry.proc_file_type > __FI_Root_Start && entry.proc_file_type < __FI_Root_End) {
                if (!strcmp(entry.name, name.characters())) {
                    return to_identifier(fsid(), PDI_Root, 0, (ProcFileType)entry.proc_file_type);
                }
            }
        }
        bool ok;
        unsigned name_as_number = name.to_uint(ok);
        if (ok) {
            bool process_exists = false;
            {
                InterruptDisabler disabler;
                process_exists = Process::from_pid(name_as_number);
            }
            if (process_exists)
                return to_identifier(fsid(), PDI_Root, name_as_number, FI_PID);
        }
        return { };
    }

    if (proc_file_type == FI_Root_sys) {
        for (size_t i = 0; i < fs().m_sys_entries.size(); ++i) {
            auto& entry = fs().m_sys_entries[i];
            if (!strcmp(entry.name, name.characters()))
                return sys_var_to_identifier(fsid(), i);
        }
        return { };
    }

    if (proc_file_type == FI_PID) {
        auto handle = ProcessInspectionHandle::from_pid(to_pid(identifier()));
        if (!handle)
            return { };
        auto& process = handle->process();
        for (auto& entry : fs().m_entries) {
            if (entry.proc_file_type > __FI_PID_Start && entry.proc_file_type < __FI_PID_End) {
                if (entry.proc_file_type == FI_PID_exe || entry.proc_file_type == FI_PID_cwd) {
                    if (entry.proc_file_type == FI_PID_exe && !process.executable_inode())
                        continue;
                    if (entry.proc_file_type == FI_PID_cwd && !process.cwd_inode())
                        continue;
                }
                if (entry.name == nullptr)
                    continue;
                if (!strcmp(entry.name, name.characters())) {
                    return to_identifier(fsid(), PDI_PID, to_pid(identifier()), (ProcFileType)entry.proc_file_type);
                }
            }
        }
        return { };
    }

    if (proc_file_type == FI_PID_fd) {
        bool ok;
        unsigned name_as_number = name.to_uint(ok);
        if (ok) {
            bool fd_exists = false;
            {
                InterruptDisabler disabler;
                if (auto* process = Process::from_pid(to_pid(identifier())))
                    fd_exists = process->file_descriptor(name_as_number);

            }
            if (fd_exists)
                return to_identifier_with_fd(fsid(), to_pid(identifier()), name_as_number);
        }
    }
    return { };
}

String ProcFSInode::reverse_lookup(InodeIdentifier child_id)
{
    ASSERT(is_directory());
    auto proc_file_type = to_proc_file_type(identifier());
    if (proc_file_type == FI_Root) {
        for (auto& entry : fs().m_entries) {
            if (child_id == to_identifier(fsid(), PDI_Root, 0, (ProcFileType)entry.proc_file_type)) {
                return entry.name;
            }
        }
        auto child_proc_file_type = to_proc_file_type(child_id);
        if (child_proc_file_type == FI_PID)
            return String::format("%u", to_pid(child_id));
        return { };
    }
    // FIXME: Implement
    ASSERT_NOT_REACHED();
    return { };
}

void ProcFSInode::flush_metadata()
{
}

ssize_t ProcFSInode::write_bytes(off_t offset, size_t size, const byte* buffer, FileDescriptor*)
{
    auto* directory_entry = fs().get_directory_entry(identifier());
    if (!directory_entry || !directory_entry->write_callback)
        return -EPERM;
    ASSERT(is_persistent_inode(identifier()));
    // FIXME: Being able to write into ProcFS at a non-zero offset seems like something we should maybe support..
    ASSERT(offset == 0);
    bool success = directory_entry->write_callback(identifier(), ByteBuffer::wrap((byte*)buffer, size));
    ASSERT(success);
    return 0;
}

bool ProcFSInode::add_child(InodeIdentifier child_id, const String& name, byte file_type, int& error)
{
    (void) child_id;
    (void) name;
    (void) file_type;
    (void) error;
    ASSERT_NOT_REACHED();
    return false;
}

bool ProcFSInode::remove_child(const String& name, int& error)
{
    (void) name;
    (void) error;
    ASSERT_NOT_REACHED();
    return false;
}

ProcFSInodeCustomData::~ProcFSInodeCustomData()
{
}

size_t ProcFSInode::directory_entry_count() const
{
    ASSERT(is_directory());
    size_t count = 0;
    traverse_as_directory([&count] (const FS::DirectoryEntry&) {
        ++count;
        return true;
    });
    return count;
}

bool ProcFSInode::chmod(mode_t, int& error)
{
    error = -EPERM;
    return false;
}

ProcFS::ProcFS()
{
    s_the = this;
    m_root_inode = adopt(*new ProcFSInode(*this, 1));
    m_entries.resize(FI_MaxStaticFileIndex);
    m_entries[FI_Root_mm] = { "mm", FI_Root_mm, procfs$mm };
    m_entries[FI_Root_mounts] = { "mounts", FI_Root_mounts, procfs$mounts };
    m_entries[FI_Root_kmalloc] = { "kmalloc", FI_Root_kmalloc, procfs$kmalloc };
    m_entries[FI_Root_all] = { "all", FI_Root_all, procfs$all };
    m_entries[FI_Root_summary] = { "summary", FI_Root_summary, procfs$summary };
    m_entries[FI_Root_cpuinfo] = { "cpuinfo", FI_Root_summary, procfs$cpuinfo};
    m_entries[FI_Root_inodes] = { "inodes", FI_Root_inodes, procfs$inodes };
    m_entries[FI_Root_dmesg] = { "dmesg", FI_Root_dmesg, procfs$dmesg };
    m_entries[FI_Root_self] = { "self", FI_Root_self, procfs$self };
    m_entries[FI_Root_sys] = { "sys", FI_Root_sys };

    m_entries[FI_PID_vm] = { "vm", FI_PID_vm, procfs$pid_vm };
    m_entries[FI_PID_vmo] = { "vmo", FI_PID_vmo, procfs$pid_vmo };
    m_entries[FI_PID_stack] = { "stack", FI_PID_stack, procfs$pid_stack };
    m_entries[FI_PID_regs] = { "regs", FI_PID_regs, procfs$pid_regs };
    m_entries[FI_PID_fds] = { "fds", FI_PID_fds, procfs$pid_fds };
    m_entries[FI_PID_exe] = { "exe", FI_PID_exe, procfs$pid_exe };
    m_entries[FI_PID_cwd] = { "cwd", FI_PID_cwd, procfs$pid_cwd };
    m_entries[FI_PID_fd] = { "fd", FI_PID_fd };
}

ProcFS::ProcFSDirectoryEntry* ProcFS::get_directory_entry(InodeIdentifier identifier) const
{
    if (to_proc_parent_directory(identifier) == PDI_Root_sys) {
        auto sys_index = to_sys_index(identifier);
        if (sys_index < m_sys_entries.size())
            return const_cast<ProcFSDirectoryEntry*>(&m_sys_entries[sys_index]);
        return nullptr;
    }
    auto proc_file_type = to_proc_file_type(identifier);
    if (proc_file_type != FI_Invalid && proc_file_type < FI_MaxStaticFileIndex)
        return const_cast<ProcFSDirectoryEntry*>(&m_entries[proc_file_type]);
    return nullptr;
}