// ARM64 host/Metal verification smoke test for Peggle 2 iOS fixes:
// 1. Verifies hardware 16 KB page compatibility in Memory / MapViewsMac / BaseHeap / MMIOHandler.
// 2. Verifies APFS case-insensitive VFS lookup and telemetry counters.
// 3. Translates and emits/compiles MSL shaders (Vertex, Fragment, Resolve/Clear, Present/UI).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ios_bridge.h"
#include "msl_prelude.h"
#include "msl_translator.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/net/socket.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static uint32_t g_mmio_reg = 0;

static uint32_t TestMmioRead(void*, void*, uint32_t addr) {
  (void)addr;
  return g_mmio_reg;
}

static void TestMmioWrite(void*, void*, uint32_t addr, uint32_t val) {
  (void)addr;
  g_mmio_reg = val;
}

static bool WriteTextFile(const std::filesystem::path& p, const std::string& content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(content.data(), std::streamsize(content.size()));
  return out.good();
}

int main(int argc, char* argv[]) {
  rex::cvar::Init(argc, argv);

  std::filesystem::path emit_dir;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--emit-metal") == 0) {
      emit_dir = argv[i + 1];
    }
  }

  // 1. Verify hardware 16 KB page size & guest arena mapping without MAP_FIXED at 0x0 or EINVAL.
  const size_t host_page = rex::memory::page_size();
  std::printf("[SMOKE] Host page_size = %zu bytes\n", host_page);
  assert(host_page == 16384 && "Expected 16 KB page size on Apple Silicon ARM64");

  rex::memory::Memory memory;
  const bool mem_ok = memory.Initialize();
  assert(mem_ok && "Memory::Initialize() must succeed on 16 KB Darwin host");
  assert(memory.virtual_membase() != nullptr);
  assert(memory.physical_membase() != nullptr);
  assert(memory.TranslatePhysical(0) == memory.physical_membase());

  // 2. Verify sub-16 KB guest operations inside pre-mapped RW arena (.xex + 4 KB virtual heaps).
  auto* xex_heap = memory.LookupHeap(0x82000000);
  assert(xex_heap != nullptr);
  assert(xex_heap->AllocFixed(
      0x82000000, 0x20000, 4096,
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  // Protect sub-section (similar to XexModule::LoadContinue) must never fail with EINVAL.
  assert(xex_heap->Protect(0x82001000, 4096, rex::memory::kMemoryProtectRead));

  auto* xex_4k_heap = memory.LookupHeap(0x90000000);
  assert(xex_4k_heap != nullptr);
  assert(xex_4k_heap->AllocFixed(
      0x90001000, 4096, 4096,
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  *memory.TranslateVirtual<uint32_t*>(0x90001000) = 0x11223344u;
  assert(xex_4k_heap->Protect(0x90001000, 4096, rex::memory::kMemoryProtectRead));
  assert(*memory.TranslateVirtual<uint32_t*>(0x90001000) == 0x11223344u);

  auto* virt_4k = memory.LookupHeap(0x00010000);
  assert(virt_4k != nullptr);
  uint32_t vaddr = 0;
  assert(virt_4k->Alloc(4096, 4096,
                        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                        &vaddr));
  assert(vaddr >= 0x00010000);

  uint32_t stack_addr = 0;
  assert(virt_4k->AllocSystemHeap(
      0x40000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true, &stack_addr));
  assert(stack_addr >= 0x30000000);

  // 3. Verify physical aliasing (0xA0000000 <-> physical_membase_ <-> 0xC0000000).
  *memory.TranslateVirtual<uint32_t*>(0xA0020000) = 0xCAFEBABEu;
  assert(*memory.TranslatePhysical<uint32_t*>(0x00020000) == 0xCAFEBABEu);
  assert(*memory.TranslateVirtual<uint32_t*>(0xC0020000) == 0xCAFEBABEu);

  // 4. Verify 64 KB MMIO window registration & read/write dispatch.
  assert(memory.AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, nullptr, TestMmioRead,
                                      TestMmioWrite));
  auto* mmio = rex::runtime::MMIOHandler::global_handler();
  assert(mmio != nullptr);
  assert(mmio->CheckStore(0x7FC80714, 0x55AA1234u));
  uint32_t read_back = 0;
  assert(mmio->CheckLoad(0x7FC80714, &read_back));
  assert(read_back == 0x55AA1234u);

  // 5. Verify VFS case-insensitive resolution & extended telemetry snapshot.
  rex::ClearCaseInsensitivePathMap();
  rex::RegisterCaseInsensitivePath("assets/Res/Images/Bg.png", "/bundle/assets/Res/Images/Bg.png");
  rex::RegisterCaseInsensitivePath("Res/Images/Bg.png", "/bundle/assets/Res/Images/Bg.png");
  rex::RegisterCaseInsensitivePath("Default.xex", "/bundle/assets/Default.xex");
  assert(rex::LookupCaseInsensitivePath("game:\\RES\\images\\bg.png") ==
         "/bundle/assets/Res/Images/Bg.png");
  assert(rex::LookupCaseInsensitivePath("d:\\default.xex") == "/bundle/assets/Default.xex");
  assert(rex::CleanGuestRelativePath("save:\\Profile\\User.dat") == "profile/user.dat");

  rex::SetBootStage(2);
  rex::SetBootStage(3);
  rex::SetBootStage(4);
  rex::SetBundleAssetCount(42);
  rex::RecordVfsOpenOk();
  rex::RecordVfsOpenFail("game:\\missing.bin");
  rex::RecordKernelFail("XSocket::Bind port=0 err=47");
  rex::RecordGuestTick();
  rex::RecordVblank();
  rex::RecordPm4Packet();
  rex::RecordGuestDraw();
  rex::RecordShaderCompiled();
  rex::SetGuestLastPC(0x8261FE50u);
  rex::SetGuestLastLR(0x8242F2C8u);
  rex::SetGuestThreadState(2u);
  assert(std::strcmp(rex::GetGuestThreadStateName(2u), "WaitObj") == 0);
  rex::SetGpuFenceWait(true);
  rex::SetThreadGuestContext(0x82315020u, 0x82315428u);
  assert(rex::GetThreadGuestPC() == 0x82315020u);
  assert(rex::GetThreadGuestLR() == 0x82315428u);
  uint64_t live_lr = 0x8259AD34ULL;
  rex::SetThreadGuestLRPtr(&live_lr);
  assert(rex::GetThreadGuestLR() == 0x8259AD34u);
  rex::SetThreadGuestLRPtr(nullptr);
  assert(rex::GetThreadGuestLR() == 0x82315428u);
  // Exercise g_last_imports[16] lock-free ring wrap (>16 entries: drop oldest 4, keep newest 16).
  static constexpr const char* kWarmupImports[4] = {
      "__imp__Warmup0", "__imp__Warmup1", "__imp__Warmup2", "__imp__Warmup3",
  };
  for (uint32_t i = 0; i < 4; ++i) {
    rex::RecordLastImport(kWarmupImports[i], i, i + 1u, i + 2u);
  }
  static constexpr const char* kRecentImports[16] = {
      "__imp__KeQueryPerformanceFrequency", "__imp__NtCreateFile",
      "__imp__NtQueryInformationFile",      "__imp__NtReadFile",
      "__imp__NtClose",                     "__imp__XamUserGetSigninState",
      "__imp__XamNotifyCreateListener",     "__imp__VdInitializeEngines",
      "__imp__VdSetDisplayMode",            "__imp__VdSwap",
      "__imp__NetDll_XNetStartup",          "__imp__NetDll_WSAStartup",
      "__imp__NetDll_socket",               "__imp__NetDll_setsockopt",
      "__imp__NetDll_getsockname",          "__imp__NetDll_bind",
  };
  for (uint32_t i = 0; i < 16; ++i) {
    rex::RecordLastImport(kRecentImports[i], 0x100u + i, 0x200u + i, 0x300u + i);
  }
  const auto pre_fault_snap = rex::GetBootTelemetrySnapshot();
  assert(pre_fault_snap.last_import_count == 16);
  assert(std::strcmp(pre_fault_snap.last_imports[0].name, kRecentImports[0]) == 0);
  assert(std::strcmp(pre_fault_snap.last_imports[15].name, "__imp__NetDll_bind") == 0);
  assert(pre_fault_snap.last_imports[15].r3 == 0x10Fu);
  assert(pre_fault_snap.last_imports[15].r4 == 0x20Fu);
  assert(pre_fault_snap.last_imports[15].return_value == 0x30Fu);

  const std::filesystem::path crash_tmp =
      std::filesystem::temp_directory_path() / "rex_ios_smoke_last_crash.txt";
  std::error_code crash_ec;
  std::filesystem::remove(crash_tmp, crash_ec);
  const std::string crash_tmp_str = crash_tmp.string();
  rex::SetCrashReportPath(crash_tmp_str.c_str());
  uint32_t live_r1 = 0x7018F8A0u;
  uint32_t live_r3 = 0x82A01234u;
  uint32_t live_r4 = 0x00000042u;
  uint32_t live_r5 = 0x00001005u;
  uint32_t live_r6 = 0x00001006u;
  uint32_t live_r7 = 0x00001007u;
  uint32_t live_r8 = 0x00001008u;
  uint32_t live_r9 = 0x00001009u;
  uint32_t live_r10 = 0x00001010u;
  uint32_t live_r11 = 0x00001011u;
  uint32_t live_r13 = 0x00001013u;
  uint32_t live_r28 = 0x414B7170u;
  uint32_t live_r29 = 0x00001029u;
  uint32_t live_r30 = 0x00001030u;
  uint32_t live_r31 = 0x00001031u;
  rex::RexGuestGprPtrs live_gprs{
      &live_r1,  &live_r3,  &live_r4,  &live_r5,  &live_r6,
      &live_r7,  &live_r8,  &live_r9,  &live_r10, &live_r11,
      &live_r13, &live_r28, &live_r29, &live_r30, &live_r31,
  };
  rex::SetThreadGuestGprPtrs(&live_gprs);
  rex::SetBuildSha("smoke1234");
  rex::RecordGuestSignalFault(11, 0x290000123ULL, 0x104008000ULL, 0x104008010ULL, 0xF9400C08u,
                              0x82315020u, 0x82315428u, "sub_82315020+0x14", "WorkerThread",
                              0x1001ULL, 0x1002ULL, 0x0ULL, 0x1004ULL);
  rex::SetThreadGuestGprPtrs(nullptr);
  // Post-fault import calls must not perturb frozen crash telemetry.
  rex::RecordLastImport("__imp__PostFaultCall", 1u, 2u, 3u);
  rex::RecordGuestVdSwapTelemetry();
  const std::string expected_gprs =
      "GPRs: r1=0x7018f8a0 r3=0x82a01234 r4=0x00000042 r5=0x00001005 "
      "r6=0x00001006 r7=0x00001007 r8=0x00001008 r9=0x00001009\n"
      "      r10=0x00001010 r11=0x00001011 r13=0x00001013 r28=0x414b7170 "
      "r29=0x00001029 r30=0x00001030 r31=0x00001031\n";
  {
    std::ifstream crash_in(crash_tmp);
    assert(crash_in.good() && "RecordGuestSignalFault must write last_crash.txt");
    std::string crash_txt((std::istreambuf_iterator<char>(crash_in)),
                          std::istreambuf_iterator<char>());
    assert(crash_txt.rfind("Build=smoke1234\nBootStage: ", 0) == 0);
    assert(crash_txt.find("BootStage: ") != std::string::npos);
    assert(crash_txt.find("GuestCPU: ") != std::string::npos);
    assert(crash_txt.find("Fault:     Sig=11 FaultAddr=0x290000123") != std::string::npos);
    assert(crash_txt.find("FaultHost: PC=0x104008000 LR=0x104008010 Insn=0xF9400C08 Sym=sub_82315020+0x14") !=
           std::string::npos);
    assert(crash_txt.find("Regs: x0=0x1001 x1=0x1002 x19=0x0 x20=0x1004 | g_r3=0x82a01234 g_r4=0x42") !=
           std::string::npos);
    assert(crash_txt.find(expected_gprs) != std::string::npos);
    assert(crash_txt.find("LastImports: 16\n  __imp__KeQueryPerformanceFrequency r3=0x00000100 r4=0x00000200 -> 0x00000300\n") !=
           std::string::npos);
    assert(crash_txt.find("  __imp__NetDll_bind r3=0x0000010f r4=0x0000020f -> 0x0000030f\n") !=
           std::string::npos);
    assert(crash_txt.find("__imp__Warmup0") == std::string::npos);
    assert(crash_txt.find("RecentFails: 2\n  VFS: game:/missing.bin\n  KRNL: XSocket::Bind port=0 err=47\n") !=
           std::string::npos);
  }
  std::filesystem::remove(crash_tmp, crash_ec);
  rex::SetCrashReportPath("");

  assert(RexIosDemangleSymbol(
             "_ZN3rex6kernel8xboxkrnl29RtlEnterCriticalSection_entryENS_7runtime5guest9ppc_ptr_tINS1_22X_RTL_CRITICAL_SECTIONEEE+0x4c")
             .find("RtlEnterCriticalSection_entry(") != std::string::npos);
  assert(RexIosDemangleSymbol("LR:_ZN3rex6system7XThread7ExecuteEv+0x1c") ==
         "LR:rex::system::XThread::Execute()+0x1c");
  assert(RexIosDemangleSymbol("sub_82315020+0x14") == "sub_82315020+0x14");

  const auto snap = rex::GetBootTelemetrySnapshot();
  {
    auto snap_mangled = snap;
    std::snprintf(snap_mangled.fault_sym, sizeof(snap_mangled.fault_sym), "%s",
                  "_ZN3rex6kernel8xboxkrnl29RtlEnterCriticalSection_entryENS_7runtime5guest9ppc_ptr_tINS1_22X_RTL_CRITICAL_SECTIONEEE+0x4c");
    std::string demangled_sym;
    const std::string formatted = RexIosFormatCrashReport(snap_mangled, &demangled_sym);
    assert(formatted.rfind("Build=smoke1234\nBootStage: ", 0) == 0);
    assert(demangled_sym.find("RtlEnterCriticalSection_entry(") != std::string::npos);
    assert(formatted.find("Sym=" + demangled_sym) != std::string::npos);
    assert(formatted.find("Regs: x0=0x1001 x1=0x1002 x19=0x0 x20=0x1004 | g_r3=0x82a01234 g_r4=0x42") !=
           std::string::npos);
    assert(formatted.find(expected_gprs) != std::string::npos);
    assert(formatted.find("LastImports: 16\n  __imp__KeQueryPerformanceFrequency r3=0x00000100 r4=0x00000200 -> 0x00000300\n") !=
           std::string::npos);
    assert(formatted.find("  __imp__NetDll_bind r3=0x0000010f r4=0x0000020f -> 0x0000030f\n") !=
           std::string::npos);
    assert(formatted.find("RecentFails: 2\n  VFS: game:/missing.bin\n  KRNL: XSocket::Bind port=0 err=47\n") !=
           std::string::npos);
  }
  assert(snap.boot_stage == 5);
  assert(snap.bundle_assets == 42);
  assert(snap.vfs_opens_ok == 1);
  assert(snap.vfs_opens_fail == 1);
  assert(std::strcmp(snap.last_fail, "missing.bin") == 0);
  assert(snap.guest_ticks == 1);
  assert(snap.vblanks == 1);
  assert(snap.pm4_packets == 1);
  assert(snap.guest_draws == 1);
  assert(snap.shaders_compiled == 1);
  assert(snap.guest_pc == 0x8261FE50u);
  assert(snap.guest_lr == 0x8242F2C8u);
  assert(snap.guest_state == 5u);  // RecordGuestSignalFault transitions state to Signal (5)
  assert(snap.gpu_fence_wait);
  assert(snap.fault_signal == 11u);
  assert(snap.fault_addr == 0x290000123ULL);
  assert(snap.fault_host_pc == 0x104008000ULL);
  assert(snap.fault_host_lr == 0x104008010ULL);
  assert(snap.fault_insn == 0xF9400C08u);
  assert(snap.fault_guest_pc == 0x82315020u);
  assert(snap.fault_guest_lr == 0x82315428u);
  assert(snap.fault_host_x0 == 0x1001ULL);
  assert(snap.fault_host_x1 == 0x1002ULL);
  assert(snap.fault_host_x19 == 0x0ULL);
  assert(snap.fault_host_x20 == 0x1004ULL);
  assert(snap.fault_guest_r3 == 0x82A01234u);
  assert(snap.fault_guest_r4 == 0x42u);
  assert(snap.fault_guest_gprs.r1 == 0x7018F8A0u);
  assert(snap.fault_guest_gprs.r13 == 0x00001013u);
  assert(snap.fault_guest_gprs.r28 == 0x414B7170u);
  assert(snap.fault_guest_gprs.r31 == 0x00001031u);
  assert(snap.last_import_count == 16);
  assert(std::strcmp(snap.last_imports[15].name, "__imp__NetDll_bind") == 0);
  assert(std::strcmp(snap.fault_sym, "sub_82315020+0x14") == 0);
  assert(std::strcmp(snap.fault_thread, "WorkerThread") == 0);
  assert(snap.vd_swaps == 1);

  // Verify recent failure ring keeps KRNL/NET entries pinned when noisy VFS probes wrap.
  rex::RecordKernelFail("NET: SendTo ret=-1 err=49");
  rex::RecordVfsOpenFail("game:\\a.bin");
  rex::RecordVfsOpenFail("game:\\b.bin");
  rex::RecordVfsOpenFail("game:\\c.bin");
  rex::RecordVfsOpenFail("game:\\d.bin");
  rex::RecordVfsOpenFail("game:\\e.bin");
  rex::RecordVfsOpenFail("game:\\f.bin");
  const auto snap2 = rex::GetBootTelemetrySnapshot();
  assert(snap2.recent_fail_count == 8);
  assert(std::strcmp(snap2.recent_fails[0], "KRNL: XSocket::Bind port=0 err=47") == 0);
  assert(std::strcmp(snap2.recent_fails[1], "KRNL: NET: SendTo ret=-1 err=49") == 0);
  assert(std::strcmp(snap2.recent_fails[7], "VFS: game:/f.bin") == 0);
  rex::RecordVfsOpenFail("game:\\g.bin");
  rex::RecordVfsOpenFail("game:\\h.bin");
  const auto snap3 = rex::GetBootTelemetrySnapshot();
  assert(snap3.recent_fail_count == 8);
  assert(std::strcmp(snap3.recent_fails[0], "KRNL: XSocket::Bind port=0 err=47") == 0);
  assert(std::strcmp(snap3.recent_fails[1], "KRNL: NET: SendTo ret=-1 err=49") == 0);
  assert(std::strcmp(snap3.recent_fails[7], "VFS: game:/h.bin") == 0);

  static_assert(sizeof(((sockaddr_in*)nullptr)->sin_family) == 1);
  assert(offsetof(sockaddr_in, sin_family) == 1);
  int udp_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(udp_fd >= 0);
  sockaddr_in bind_addr{};
  bind_addr.sin_len = sizeof(sockaddr_in);
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind_addr.sin_port = 0;
  assert(::bind(udp_fd, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) == 0);
  ::close(udp_fd);

  // Privileged Xbox LIVE VDP port 1000 (< 1024) fails EACCES for non-root on Darwin; verify fallback.
  int priv_direct_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(priv_direct_fd >= 0);
  sockaddr_in priv_addr = bind_addr;
  priv_addr.sin_port = htons(1000);
  int direct_rc = ::bind(priv_direct_fd, reinterpret_cast<const sockaddr*>(&priv_addr), sizeof(priv_addr));
  if (direct_rc < 0) {
    assert(errno == EACCES || errno == EPERM);
  }
  ::close(priv_direct_fd);

  int priv_fb_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(priv_fb_fd >= 0);
  uint16_t bound_port = 0;
  int bind_err = 0;
  assert(rex::net::socket_bind_ipv4(priv_fb_fd, AF_INET, htonl(INADDR_LOOPBACK), htons(1000),
                                     &bound_port, &bind_err) == 0);
  assert(bound_port != 0);
  ::close(priv_fb_fd);

  // Also verify SO_BROADCAST + INADDR_ANY port 1000 (sub_822FC130) binds cleanly or falls back to loopback.
  int any_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(any_fd >= 0);
  int bcast = 1;
  ::setsockopt(any_fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
  uint16_t any_port = 0;
  int any_err = 0;
  assert(rex::net::socket_bind_ipv4(any_fd, AF_INET, htonl(INADDR_ANY), htons(1000),
                                     &any_port, &any_err) == 0);
  assert(any_port != 0);
  uint16_t sock_fam = 0;
  uint32_t sock_addr_be = 0;
  uint16_t sock_port_be = 0;
  int gs_err = 0;
  assert(rex::net::socket_getsockname_ipv4(any_fd, &sock_fam, &sock_addr_be, &sock_port_be,
                                           &gs_err) == 0);
  assert(sock_fam == AF_INET);
  assert(ntohs(sock_port_be) == any_port);
  sockaddr_in self_dst{};
  self_dst.sin_len = sizeof(sockaddr_in);
  self_dst.sin_family = AF_INET;
  self_dst.sin_addr.s_addr = sock_addr_be ? sock_addr_be : htonl(INADDR_LOOPBACK);
  self_dst.sin_port = sock_port_be;
  const char probe[4] = {0, 0, 0, 0};
  assert(::sendto(any_fd, probe, sizeof(probe), 0,
                  reinterpret_cast<const sockaddr*>(&self_dst), sizeof(self_dst)) == 4);
  ::close(any_fd);

  // 5b. Verify quick-tap classifier (< 250 ms and < 18 pt travel) & ring allocator + bg pause.
  assert(RexIosIsQuickTap(100.0f, 100.0f, 110.0f, 105.0f, 120));
  assert(!RexIosIsQuickTap(100.0f, 100.0f, 100.0f, 100.0f, 250));   // >= 250 ms
  assert(!RexIosIsQuickTap(100.0f, 100.0f, 118.0f, 100.0f, 100));   // >= 18 pt drag
  {
    uint32_t cursor = 12, off1 = 999, off2 = 999;
    assert(RexIosRingBumpAlloc(cursor, 80, 1024, off1));
    assert(off1 == 256 && cursor == 336);
    assert(RexIosRingBumpAlloc(cursor, 512, 1024, off2));
    assert(off2 == 512 && cursor == 1024);
    assert(!RexIosRingBumpAlloc(cursor, 256, 1024, off2));
    assert(!RexIosRingBumpAlloc(cursor, 0, 1024, off2));
    uint32_t near_end = 1020;
    assert(!RexIosRingBumpAlloc(near_end, 16, 1024, off2));
    uint32_t overflow_cursor = UINT32_MAX - 10u;
    assert(!RexIosRingBumpAlloc(overflow_cursor, 64, 1024, off2));
  }
  rex::SetAppBackgroundPaused(true);
  assert(rex::IsAppBackgroundPaused());
  rex::SetAppBackgroundPaused(false);
  assert(!rex::IsAppBackgroundPaused());

  // 6. Verify Metal shaders (Resolve, Present/UI, Prelude + Vertex Entry + Translated Pixel Entry).
  rex_ios::MslShaderInfo ps_info{};
  ps_info.isPixel = true;
  ps_info.colorTargets = 1u;
  ps_info.textureSlots = 1u;
  ps_info.inputsRead = 1u;
  const std::string vs_msl =
      std::string(rex_ios::kMslPrelude) +
      "static void xe_vs_main(thread XeState& st, constant float4* c, constant uint* boolLoop, "
      "constant uint* fetch, device const uint* mem, device const uint* memHi, uint splitWord)\n"
      "{\n"
      "    st.position = c[0] + float4(as_type<float>(xe_mem_load(mem, memHi, splitWord, 0u)), 0.0, 0.0, 1.0);\n"
      "    st.o[0] = xe_vfetch(mem, memHi, splitWord, fetch, 0, 0u, 38u, false, false, 0u, 0);\n"
      "}\n" +
      rex_ios::kMslVertexEntry;
  const std::string ps_msl =
      std::string(rex_ios::kMslPrelude) +
      "static void xe_ps_main(thread XeState& st, constant float4* c, constant uint* boolLoop, "
      "thread XePixelIO& io, texture2d<float> t0, sampler s0)\n"
      "{\n"
      "    st.color[0] = t0.sample(s0, st.r[0].xy) * c[0];\n"
      "}\n" +
      rex_ios::FragmentEntry(ps_info);

  if (!emit_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(emit_dir, ec);
    assert(WriteTextFile(emit_dir / "resolve.metal", rex_ios::kResolveShaders));
    assert(WriteTextFile(emit_dir / "present_ui.metal", rex_ios::kPresentAndUiShaders));
    assert(WriteTextFile(emit_dir / "xe_vs.metal", vs_msl));
    assert(WriteTextFile(emit_dir / "xe_ps.metal", ps_msl));
    std::printf("[SMOKE] Emitted .metal shaders to %s\n", emit_dir.string().c_str());
  }

  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev) {
      auto compile = [&](const char* label, const char* src) {
        NSError* err = nil;
        MTLCompileOptions* opt = [MTLCompileOptions new];
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:opt error:&err];
        if (!lib) {
          std::fprintf(stderr, "Metal compile failed (%s): %s\n", label,
                       err ? err.localizedDescription.UTF8String : "unknown");
        }
        assert(lib != nil);
      };
      compile("resolve", rex_ios::kResolveShaders);
      compile("present_ui", rex_ios::kPresentAndUiShaders);
      compile("xe_vs", vs_msl.c_str());
      compile("xe_ps", ps_msl.c_str());
      id<MTLBuffer> shared_watcher_buf =
          [dev newBufferWithLength:320u * 180u * 4u options:MTLResourceStorageModeShared];
      assert(shared_watcher_buf != nil && [shared_watcher_buf contents] != nullptr);
      std::printf("[SMOKE] Runtime Metal device '%s' compiled all 4 MSL libraries cleanly\n",
                  dev.name.UTF8String);
    } else {
      std::printf("[SMOKE] Headless runner (no MTLCreateSystemDefaultDevice); relying on xcrun metal\n");
    }
  }

  std::printf("[SMOKE] All iOS 16KB arena, MMIO, VFS, Telemetry & MSL checks PASSED.\n");
  return 0;
}
