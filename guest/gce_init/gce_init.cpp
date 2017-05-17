/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <api_level_fixes.h>

#define GCE_INIT_DEBUG 0
#define LOWER_SYSTEM_MOUNT_POINT "/var/system_lower"
#define UPPER_SYSTEM_MOUNT_POINT "/var/system_upper"

#include <map>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include "gce_fs.h"

#include <cutils/properties.h>
#include <zlib.h>
#include <api_level_fixes.h>
#include <AutoResources.h>
#include <DisplayProperties.h>
#include <GceMetadataAttributes.h>
#include <GetPartitionNum.h>
#include <GceResourceLocation.h>
#include <InitialMetadataReader.h>
#include <MetadataQuery.h>
#include <UnpackRamdisk.h>
#include <SharedFD.h>

#include <gce_network/logging.h>
#include <gce_network/namespace_aware_executor.h>
#include <gce_network/netlink_client.h>
#include <gce_network/network_interface_manager.h>
#include <gce_network/network_namespace_manager.h>
#include <gce_network/sys_client.h>

#include "environment_setup.h"
#include "properties.h"

#if GCE_PLATFORM_SDK_AFTER(J_MR2)
#define INIT_PROPERTIES 1
// We need this to make shared libraries work on K.
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#endif

#include <private/android_filesystem_config.h>
#include <namespace_constants.h>

#if defined(__LP64__)
#define LIBRARY_PATH_SYSTEM "/system/lib64/hw/"
#define LIBRARY_PATH_VENDOR "/vendor/lib64/hw/"
#define TARGET_LIB_PATH_RIL "/target/system/lib64/libgce_x86-ril%s.so"
#define TARGET_LIB_PATH_HW_COMPOSER \
    "/target/system/lib64/hw/hwcomposer.gce_x86%s.so"
#else
#define LIBRARY_PATH_SYSTEM "/system/lib/hw/"
#define LIBRARY_PATH_VENDOR "/vendor/lib/hw/"
#define TARGET_LIB_PATH_RIL "/target/system/lib/libgce_x86-ril%s.so"
#define TARGET_LIB_PATH_HW_COMPOSER \
    "/target/system/lib/hw/hwcomposer.gce_x86%s.so"
#endif

using avd::InitialMetadataReader;
using avd::UniquePtr;
using avd::EnvironmentSetup;
using avd::kCloneNewNet;
using avd::NamespaceAwareExecutor;
using avd::NetlinkClient;
using avd::NetworkInterfaceManager;
using avd::NetworkNamespaceManager;
using avd::SysClient;

namespace {
// Linux device major and minor numbers can be found here:
// http://lxr.free-electrons.com/source/Documentation/devices.txt
struct DeviceSpec {
  int major;
  int minor;
  int mode;
  const char* path;
};

const DeviceSpec simple_char_devices[] = {
  {1, 3, 0666, "/dev/null"},
  {1, 8, 0666, "/dev/random"},
  {1, 9, 0666, "/dev/urandom"},
  {1, 11, 0644, "/dev/kmsg"},
  {10, 237, 0600, "/dev/loop-control"}
};

const char kDevBlockDir[] = "/dev/block";
const char kCustomInitFileName[] = "/target/init.metadata.rc";
const char kMetadataPropertiesFileName[] = "/target/metadata_properties.rc";
const char kEmergencyShell[] = "/system/bin/sh";
const char kMultibootDevice[] = "/dev/block/sda";
const int kMultibootPartition = 1;
const char kDefaultPartitionsPath[] = "/target/partitions";

const char kNestedVMParameter[] = "AVD_NESTED_VM";

#define AVD_WARN "AVD WARNING"

// Place all files and folders you need bind-mounted here.
// All files and folders are required to be part of the (target) dir structure.
// Examples:
// - { "/fstab.gce_x86.template", "/fstab" }
//   bind-mounts file /fstab.gce_x86.template to /fstab.
//   The /fstab file will be created and, for debugging purposes, will be
//   initialized with the path of the source file.
// - { "/system", "/system.2" }
//   bind-mounts directory /system to /system.2.
//   The /system.2 directory will be created.
const char* kBindFiles[][2] = {
  { NULL, NULL }
};

}  // namespace


class Container {
 public:
  enum DeviceType {
    kDeviceTypeUnknown,
    kDeviceTypeWifi,
    kDeviceType3G,
  };

  Container()
      : device_type_(kDeviceTypeUnknown),
        is_nested_vm_(false) {}

  ~Container() {}

  // Initializes minimum environment needed to launch basic commands.
  // This section should eventually be deleted as we progress with containers.
  bool InitializeMinEnvironment(AutoFreeBuffer* error);

  // Managers require a minimum working environment to be created.
  const char* CreateManagers();

  const char* InitializeNamespaces();
  const char* ConfigureNetworkCommon();
  const char* ConfigureNetworkMobile();

  const char* FetchMetadata();
  const char* InitTargetFilesystem();
  const char* BindFiles();
  const char* ApplyCustomization();
  const char* PivotToNamespace(const char* name);

  const char* CleanUp();

 private:
  const char* ApplyCustomInit();
  const char* ApplyMetadataProperties();
  const char* Bind(const char* source, const char* target);
  const char* SelectVersion(
      const char* name,
      const char* version,
      const char* name_pattern);

  UniquePtr<SysClient> sys_client_;
  UniquePtr<NetlinkClient> nl_client_;
  UniquePtr<NetworkNamespaceManager> ns_manager_;
  UniquePtr<NetworkInterfaceManager> if_manager_;
  UniquePtr<NamespaceAwareExecutor> executor_;
  UniquePtr<EnvironmentSetup> setup_;
  InitialMetadataReader* reader_;
  std::string android_version_;
  DeviceType device_type_;
  bool is_nested_vm_;

  Container(const Container&);
  Container& operator= (const Container&);
};


static const char* MountTmpFs(const char* mount_point, const char* size) {
  if (gce_fs_prepare_dir(mount_point, 0700, 0, 0) == 0) {
    if (mount("tmpfs", mount_point, "tmpfs", MS_NOSUID, size) == 0) {
      return NULL;
    } else {
      KLOG_ERROR(LOG_TAG, "Could not mount tmpfs at %s: %d (%s)",
                 mount_point, errno, strerror(errno));
    }
  } else {
    KLOG_ERROR(LOG_TAG, "Could not prepare dir %s: %d (%s)",
               mount_point, errno, strerror(errno));
  }

  return "tmpfs mount failed.";
}

bool CreateDeviceNode(const char* name, int flags, int major, int minor) {
  dev_t dev = makedev(major, minor);
  mode_t old_mask = umask(0);
  int rval = TEMP_FAILURE_RETRY(mknod(name, flags, dev));
  umask(old_mask);
  if (rval == -1) {
    KLOG_ERROR(LOG_TAG, "mknod failed for %s: (%s)\n", name, strerror(errno));
    return false;
  }
  return true;
}

bool CreateBlockDeviceNodes() {
  AutoCloseFILE f(fopen("/proc/partitions", "r"));
  char line[160];
  char device[160];
  if (!f) {
    KLOG_ERROR(LOG_TAG, "open of /proc/partitions failed: (%s)\n",
               strerror(errno));
    return false;
  }

  if (gce_fs_prepare_dir(kDevBlockDir, 0700, 0, 0) == -1) {
    KLOG_INFO(LOG_TAG, "gs_fs_prepare_dir(%s) failed: (%s)\n",
              kDevBlockDir, strerror(errno));
    return false;
  }

  int major, minor;
  long long blocks;
  bool found = false;
  while (!found && fgets(line, sizeof(line), f)) {
    int fields = sscanf(line, "%d%d%lld%s", &major, &minor, &blocks, device);
    if (fields == 4) {
      AutoFreeBuffer dev_path;
      dev_path.PrintF("%s/%s", kDevBlockDir, device);
      if (!CreateDeviceNode(
          dev_path.data(), S_IFBLK | S_IRUSR | S_IWUSR, major, minor)) {
        return false;
      }
    }
  }
  return true;
}

// Mounts a filesystem.
// Returns true if the mount happened.
bool MountFilesystem(
    const char* fs, const char* disk, long partition_num, const char* dir,
    unsigned long mount_flags = MS_RDONLY | MS_NODEV) {
  AutoFreeBuffer temp_dev;
  if (gce_fs_prepare_dir(dir, 0700, 0, 0) == -1) {
    KLOG_ERROR(LOG_TAG, "gs_fs_prepare_dir(%s) failed: %s\n",
               dir, strerror(errno));
    return false;
  }
  if (disk && *disk) {
    if (partition_num) {
      temp_dev.PrintF("%s%ld", disk, partition_num);
    } else {
      temp_dev.SetToString(disk);
    }
  }
  if (TEMP_FAILURE_RETRY(
          mount(temp_dev.data(), dir, fs, mount_flags, NULL)) == -1) {
    KLOG_ERROR(LOG_TAG, "mount of %s failed: %s\n", dir, strerror(errno));
    return false;
  }
  return true;
}

// Copies a file.
// Returns true if the copy succeeded.
static bool CopyFile(const char* in_path, const char* out_path) {
  AutoCloseFILE in(fopen(in_path, "rb"));
  if (in.IsError()) {
    KLOG_ERROR(LOG_TAG, "unable to open input file %s: %s\n",
               in_path, strerror(errno));
    return false;
  }
  AutoCloseFILE out(fopen(out_path, "wb"));
  if (out.IsError()) {
    KLOG_ERROR(LOG_TAG, "unable to open output file %s: %s\n",
               out_path, strerror(errno));
    return false;
  }
  if (!out.CopyFrom(in)) {
    return false;
  }
  return true;
}

bool IsNestedVM() {
  AutoFreeBuffer cmdline;
  avd::SharedFD cmdlinefd = avd::SharedFD::Open("/proc/cmdline", O_RDONLY, 0);

  // One doesn't simply stat a /proc file.
  // On more serious note: yes, I'm making an assumption here regarding length
  // of kernel command line - that it's no longer than 16k.
  // Linux allows command line length to be up to MAX_ARG_STRLEN long, which
  // is essentially 32 * PAGE_SIZE (~256K). I don't think we'll get that far any
  // time soon.
  if (!cmdlinefd->IsOpen()) {
    KLOG_WARNING(LOG_TAG, "Unable to read /proc/cmdline: %s\n",
                 cmdlinefd->StrError());
    return false;
  }

  // 16k + 1 padding zero.
  cmdline.Resize(16384 + 1);
  cmdlinefd->Read(cmdline.data(), cmdline.size());
  KLOG_WARNING(LOG_TAG, "%s\n", cmdline.data());
  return (strstr(cmdline.data(), kNestedVMParameter) != NULL);
}

static bool MountSystemPartition(
    const char* partitions_path, const char* mount_point, bool is_nested_vm) {
  mode_t save = umask(0);
  int result = TEMP_FAILURE_RETRY(mkdir(mount_point, 0777));
  umask(save);
  if ((result == -1) && (errno != EEXIST)) {
    KLOG_ERROR(LOG_TAG, "skipping %s: mkdir failed: %s\n",
               mount_point, strerror(errno));
    return false;
  }

  // Fixed fallback values, used with nested virtualization.
  const char* boot_device = "/dev/block/vdb";
  long system_partition_num = 0;

  if (!is_nested_vm) {
    boot_device = kMultibootDevice;
    system_partition_num = GetPartitionNum("system", partitions_path);
    if (system_partition_num == -1) {
      KLOG_ERROR(LOG_TAG, "unable to find system partition\n");
      return false;
    }
  }

  if (!MountFilesystem(
      "ext4", boot_device, system_partition_num, mount_point)) {
    KLOG_ERROR(LOG_TAG, "unable to mount system partition %s%ld\n",
               boot_device, system_partition_num);
    return false;
  }

  return true;
}


// Attempt to mount a system overlay, returning true only if the overlay
// was created.
// The overlay will be mounted read-only here to avoid a serious security
// issues: mounting the upper filesystem read-write would allow attackers
// to modify the "read-only" overlay via the upper mount point.
//
// This means that we need to pass additional data to adb to allow remount to
// work as expected.
// We use the unused device parameter to pass a hint to adb to coordinate the
// remount. In addition, we create a directory to allow adb to construct a
// writable overlay that will be bound to /system.
static bool MountSystemOverlay(
    const InitialMetadataReader& reader, bool is_nested_vm) {
  const char* system_overlay_device = reader.GetValueForKey(
      GceMetadataAttributes::kSystemOverlayDeviceKey);
  if (!system_overlay_device) {
    KLOG_INFO(LOG_TAG, "No system overlay device.\n");
    return false;
  }
  if (!MountFilesystem("ext4", system_overlay_device, 0,
                       UPPER_SYSTEM_MOUNT_POINT)) {
    KLOG_INFO(LOG_TAG, "Could not mount overlay device %s.\n",
              system_overlay_device);
    return false;
  }
  if (!MountSystemPartition(
      kDefaultPartitionsPath, LOWER_SYSTEM_MOUNT_POINT, is_nested_vm)) {
    KLOG_INFO(LOG_TAG, "Could not mount %s from %s at %s.\n",
              kMultibootDevice, kDefaultPartitionsPath,
              LOWER_SYSTEM_MOUNT_POINT);
    return false;
  }
  gce_fs_prepare_dir("/target/system", 0700, 0, 0);
  const char* const remount_hint =
     "uppermntpt=" UPPER_SYSTEM_MOUNT_POINT ","
     "upperdir=" UPPER_SYSTEM_MOUNT_POINT "/data,"
     "workdir=" UPPER_SYSTEM_MOUNT_POINT "/work,"
     "lowerdir=" LOWER_SYSTEM_MOUNT_POINT;
  if (mount(remount_hint, "/target/system", "overlay", MS_RDONLY | MS_NODEV,
            "lowerdir=" UPPER_SYSTEM_MOUNT_POINT "/data:"
                        LOWER_SYSTEM_MOUNT_POINT) == -1) {
    KLOG_ERROR(LOG_TAG, "Overlay mount failed, falling back to base system: %s\n",
               strerror(errno));
    return false;
  }
  if (gce_fs_prepare_dir("/target/system_rw", 0700, 0, 0) == -1) {
    KLOG_ERROR(LOG_TAG, "Failed to create /system_rw. adb remount will fail\n");
  }
  return true;
}

class BootPartitionMounter {
 public:
  BootPartitionMounter(bool is_nested_vm)
      : is_nested_vm_(is_nested_vm),
        is_mounted_(false) {
    if (!is_nested_vm_) {
      // All mounts of disk partitions must be read-only.
      is_mounted_ = MountFilesystem(
          "ext4", kMultibootDevice, kMultibootPartition, multiboot_location_);
    }
  }

  ~BootPartitionMounter() {
    if (!is_nested_vm_ && is_mounted_) {
      umount2(multiboot_location_, MNT_FORCE);
    }
  }

  bool IsSuccess() const {
    return is_mounted_ || is_nested_vm_;
  }

 private:
  bool is_nested_vm_;
  bool is_mounted_;
  const char* const multiboot_location_ = "/boot";
};

bool Init(Container* container, AutoFreeBuffer* error) {
  if (!container->InitializeMinEnvironment(error)) {
    return false;
  }

  const char* res = container->CreateManagers();
  if (res) {
    error->SetToString(res);
    return false;
  }

  res = container->InitializeNamespaces();
  if (res) {
    error->SetToString(res);
    return false;
  }

  res = container->PivotToNamespace(NetworkNamespaceManager::kOuterNs);
  if (res) {
    error->SetToString(res);
    return false;
  }

  res = container->ConfigureNetworkCommon();
  if (res) {
    error->SetToString(res);
    return false;
  }

  // Remove our copy of /init to allow decompressing the target init process.
  if (TEMP_FAILURE_RETRY(rename("/init", "/gce-init")) == -1) {
    KLOG_ERROR(LOG_TAG, "Failed to move /init: %s\n", strerror(errno));
  }

  res = container->FetchMetadata();
  if (res) {
    error->SetToString(res);
    return false;
  }

  res = container->PivotToNamespace(NetworkNamespaceManager::kAndroidNs);
  if (res) {
    error->SetToString(res);
    return false;
  }

  res = container->InitTargetFilesystem();
  if (res) {
    error->SetToString(res);
    return false;
  }

  res = container->ApplyCustomization();
  if (res) {
    error->SetToString(res);
    return false;
  }

  KLOG_INFO(LOG_TAG, "Pivoting to Android Init\n");

  res = container->CleanUp();
  if (res) {
    error->SetToString(res);
    return false;
  }

  // Chain to the Android init process.
  int rval = TEMP_FAILURE_RETRY(execl("/init", "/init", NULL));
  if (rval == -1) {
    KLOG_ERROR(LOG_TAG, "execl failed: %d (%s)\n", errno, strerror(errno));
    error->SetToString("Could not exec init.");
    return false;
  }

  error->SetToString("exec finished unexpectedly.");
  return false;
}

bool Container::InitializeMinEnvironment(AutoFreeBuffer* error) {
  // Set up some initial enviromnent stuff that we need for reliable shared
  // libraries.
  if (!MountFilesystem("proc", NULL, 0, "/proc", 0)) {
    error->SetToString("Could not mount initial /proc.");
    return false;
  }

  if (!MountFilesystem("sysfs", NULL, 0, "/sys", 0)) {
    error->SetToString("Could not mount initial /sys.");
    return false;
  }

  // Set up tmpfs partitions for /dev. Normally Android's init would handle
  // this. However, processes are going to retain opens to /dev/__properties__,
  // and if it's on the root filesystem we won't be able to remount it
  // read-only later in the boot.
  //
  // We need to initialize the properties because K's bionic will
  // crash if they're not in place.
  //
  // This works because init's attempt to mount a tmpfs later will silently
  // overlay the existing mount.
  const char* res;
  res = MountTmpFs("/dev", "mode=0755");
  if (res) {
    error->SetToString(res);
    return false;
  }

  // Set up tmpfs partitions for /var
  res = MountTmpFs("/var", "mode=0755");
  if (res) {
    error->SetToString(res);
    return false;
  }

  for (size_t i = 0;
       i < sizeof(simple_char_devices) / sizeof(simple_char_devices[0]); ++i) {
    if (!CreateDeviceNode(
        simple_char_devices[i].path, S_IFCHR | simple_char_devices[i].mode,
        simple_char_devices[i].major, simple_char_devices[i].minor)) {
      error->PrintF("Could not create %s", simple_char_devices[i].path);
      return false;
    }
  }

  if (!CreateBlockDeviceNodes()) {
    error->SetToString("Could not create block device nodes.");
    return false;
  }

  is_nested_vm_ = IsNestedVM();

  // Mount the boot partition so we can get access the configuration and
  // ramdisks there.
  // Unmount this when we're done so that this doesn't interefere with mount
  // namespaces later.
  {
    BootPartitionMounter boot_mounter(is_nested_vm_);

    if (!boot_mounter.IsSuccess()) {
      error->SetToString("Could not mount multiboot /boot partition.");
      return false;
    }

    // Mount the default system partition so we can issue a DHCP request.
    if (!MountSystemPartition(
        "/boot/targets/default/partitions", "/system", is_nested_vm_)) {
      error->SetToString("Could not mount multiboot /system partition.");
      return false;
    }
  }

  if (setenv("LD_LIBRARY_PATH", LIBRARY_PATH_SYSTEM ":" LIBRARY_PATH_VENDOR,
             1) == -1) {
    error->SetToString("Failed to set LD_LIBRARY_PATH.");
    return false;
  }

  if (gce_fs_mkdirs("/data", 0755) != 0) {
    error->SetToString("Could not create /data folder.");
    return false;
  }

  // Required by KK bionic, which would crash if properties were not initialized
  // at boot time.
#if GCE_PLATFORM_SDK_AFTER(J_MR2)
  __system_property_area_init();
#endif

  return true;
}

const char* Container::CreateManagers() {
  sys_client_.reset(SysClient::New());
  if (!sys_client_.get()) return "Unable to create sys client.";

  nl_client_.reset(NetlinkClient::New(sys_client_.get()));
  if (!nl_client_.get()) return "Unable to create netlink client.";

  ns_manager_.reset(NetworkNamespaceManager::New(sys_client_.get()));
  if (!ns_manager_.get()) return "Unable to create namespace manager.";

  if_manager_.reset(NetworkInterfaceManager::New(nl_client_.get(), ns_manager_.get()));
  if (!if_manager_.get()) return "Unable to create interface manager.";

  executor_.reset(NamespaceAwareExecutor::New(ns_manager_.get(), sys_client_.get()));
  if (!executor_.get()) return "Unable to create executor.";

  setup_.reset(new EnvironmentSetup(
      executor_.get(), ns_manager_.get(), if_manager_.get(), sys_client_.get()));

  return NULL;
}

const char* Container::InitializeNamespaces() {
  if (!setup_->CreateNamespaces())
    return "Could not create namespaces.";

  return NULL;
}

const char* Container::ConfigureNetworkCommon() {
  // Allow the scripts started by DHCP to update the MTU.
  if (gce_fs_mkdirs(OUTER_INTERFACE_CONFIG_DIR,
                    S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) == -1) {
    KLOG_ERROR(LOG_TAG, "Unable to create %s: %s\n", OUTER_INTERFACE_CONFIG_DIR,
               strerror(errno));
    return "Could not create host interface env folder.";
  }

  if (!setup_->ConfigureNetworkCommon()) {
    return "Failed to configure common network.";
  }

  if (is_nested_vm_) {
    if (!setup_->ConfigurePortForwarding())
      return "Failed to configure port forwarding.";
  }

  return NULL;
}

const char* Container::ConfigureNetworkMobile() {
  KLOG_INFO(LOG_TAG, "Configuring mobile network\n");
  if (!setup_->ConfigureNetworkMobile())
    return "Failed to configure mobile network.";

  return NULL;
}

const char* Container::FetchMetadata() {
  // Now grab the metadata that will tell us which image we need to boot.
  AutoFreeBuffer buffer;

  // Metadata server is offering metadata only within Android namespace.
  // Flip current namespace temporarily to construct the reader.
  if (sys_client_->SetNs(
      ns_manager_->GetNamespaceDescriptor(
          NetworkNamespaceManager::kAndroidNs), kCloneNewNet) < 0) {
    KLOG_ERROR(LOG_TAG, "Failed to switch namespace: %s\n", strerror(errno));
    return "Could not switch namespace to initiate metadata connection.";
  }

  // Wait for initial metadata.
  // It may not be instantly available; keep looping until it pops up.
  MetadataQuery* query = MetadataQuery::New();
  KLOG_INFO(LOG_TAG, "Waiting for initial metadata...\n");
  while (!query->QueryServer(&buffer)) {
    usleep(100 * 1000);
  }
  KLOG_INFO(LOG_TAG, "Metadata ready.\n");
  delete query;

  reader_ = InitialMetadataReader::getInstance();
  // Return to original network namespace.
  if (sys_client_->SetNs(
      ns_manager_->GetNamespaceDescriptor(
          NetworkNamespaceManager::kOuterNs), kCloneNewNet) < 0) {
    KLOG_ERROR(LOG_TAG, "Failed to switch namespace: %s\n", strerror(errno));
    return "Could not switch namespace after initiating metadata connection.";
  }

  const char* version = reader_->GetValueForKey(
      GceMetadataAttributes::kAndroidVersionKey);
  if (version) {
    android_version_ = version;
  } else {
    android_version_ = "default";
  }

  KLOG_INFO(LOG_TAG, "Booting android_version=%s\n", android_version_.c_str());

  return NULL;
}

const char* Container::InitTargetFilesystem() {
  // We need to have our /target folder mounted in order to re-mount root.
  // To achieve this, we keep two folders:
  // - /target_mount, a directory, where everything is kept,
  // - /target, bind-mounted to /target_mount.
  if (gce_fs_mkdirs("/target_mount", 0755) != 0)
    return "Could not create /target_mount folder.";
  if (gce_fs_mkdirs("/target", 0755) != 0)
    return "Could not create /target folder.";
  if (mount("/target_mount", "/target", NULL, MS_BIND, NULL))
    return "Could not mount /target_mount.";

  if (gce_fs_mkdirs("/target/boot", 0755) != 0)
    return "Could not create /target/boot folder.";
  if (gce_fs_mkdirs("/target/system", 0755) != 0)
    return "Could not create /target/system folder.";
  if (gce_fs_mkdirs("/target/proc", 0755) != 0)
    return "Could not create /target/proc folder.";
  if (gce_fs_mkdirs("/target/sys", 0755) != 0)
    return "Could not create /target/sys folder.";
  if (gce_fs_mkdirs("/target/var", 0755) != 0)
    return "Could not create /target/var folder.";
  if (gce_fs_mkdirs("/target" EPHEMERAL_FS_BLOCK_DIR, 0755) != 0)
    return "Could not create /target" EPHEMERAL_FS_BLOCK_DIR " folder.";

  // Set up tmpfs for the ephemeral block devices
  // This leaves 512MB of RAM for the system on a n1-standard-1 (our smallest
  // supported configuration). 512MB is the minimum supported by
  // KitKat. The Nexus S shipped in this configuration.
  const char* res;
  res = MountTmpFs("/target" EPHEMERAL_FS_BLOCK_DIR, "size=86%");
  if (res) return res;

  {
    BootPartitionMounter boot_mounter(is_nested_vm_);

    if (!boot_mounter.IsSuccess()) {
      return "Could not mount multiboot /boot partition.";
    }

    if (!is_nested_vm_) {
      // Unpack the RAM disk here because gce_mount_hander needs the fstab template
      AutoFreeBuffer ramdisk_path;
      ramdisk_path.PrintF(
          "/boot/targets/%s/ramdisk", android_version_.c_str());
      UnpackRamdisk(ramdisk_path.data(), "/target");

      // Place the partitions file in root. It will be needed by gce_mount_handler
      AutoFreeBuffer partitions_path;
      partitions_path.PrintF(
          "/boot/targets/%s/partitions", android_version_.c_str());
      CopyFile(partitions_path.data(), kDefaultPartitionsPath);
    } else {
      UnpackRamdisk("/dev/block/vda", "/target");
    }
  }

  if (!MountSystemOverlay(*reader_, is_nested_vm_) &&
      !MountSystemPartition(
          kDefaultPartitionsPath, "/target/system", is_nested_vm_))
    return "Unable to mount /target/system.";


  return NULL;
}

const char* Container::BindFiles() {
  for (int index = 0; kBindFiles[index][0] != NULL; ++index) {
    AutoFreeBuffer source, target;

    source.PrintF("/target%s", kBindFiles[index][0]);
    target.PrintF("/target%s", kBindFiles[index][1]);

    const char* res = Bind(source.data(), target.data());
    if (res) return res;
  }

  const char* res = NULL;

  res = SelectVersion(
      "RIL", GceMetadataAttributes::kRilVersionKey,
      TARGET_LIB_PATH_RIL);
  if (res) return res;

  res = SelectVersion(
      "HWComposer", GceMetadataAttributes::kHWComposerVersionKey,
      TARGET_LIB_PATH_HW_COMPOSER);
  if (res) return res;

  res = SelectVersion(
      "VNC", GceMetadataAttributes::kVncServerVersionKey,
      "/target/system/bin/vnc_server%s");
  if (res) { return res; }

  return NULL;
}

const char* Container::SelectVersion(
    const char* name,
    const char* metadata_key,
    const char* pattern) {
  const char* version = reader_->GetValueForKey(metadata_key);
  if (!version) return NULL;

  char default_version[PATH_MAX];
  char selected_version[PATH_MAX];

  snprintf(&default_version[0], sizeof(default_version), pattern, "");

  struct stat sb;
  if (stat(default_version, &sb) < 0) {
    KLOG_WARNING(AVD_WARN, "Ignoring %s variant setting %s: not applicable.\n",
                 name, version);
    return NULL;
  }

  if (!version || !strcmp(version, "DEFAULT")) {
    // No change.
    return NULL;
  } else if (!strcmp(version, "TESTING")) {
    snprintf(&selected_version[0], sizeof(selected_version),
             pattern, "-testing");
  } else if (!strcmp(version, "DEPRECATED")) {
    snprintf(&selected_version[0], sizeof(selected_version),
             pattern, "-deprecated");
  } else {
    KLOG_WARNING(AVD_WARN, "Variant %s not valid for %s. Using default.\n",
                 version, name);
    return NULL;
  }

  // So, user specified a different variant of module, but this variant is
  // not explicitly specified.
  if (stat(selected_version, &sb) < 0) {
    KLOG_WARNING(AVD_WARN, "Ignoring %s variant setting %s: not available.\n",
                 name, version);
    return NULL;
  }

  KLOG_NOTICE(LOG_TAG, "Switching %s to %s variant\n", name, version);
  return Bind(selected_version, default_version);
}

const char* Container::Bind(const char* source, const char* target) {
  struct stat sb, tb;
  if (stat(source, &sb) < 0) {
    KLOG_ERROR(LOG_TAG, "Could not stat bind file %s: %s\n",
               source, strerror(errno));
    return "Could not find bind source.";
  }

  if (stat(target, &tb) < 0) {
    KLOG_ERROR(LOG_TAG, "Could not bind-mount to target %s: %s.\n",
               target, strerror(errno));
    return "Could not find bind target.";
  }

  // Create file / folder to which we will bind-mount source file / folder.
  if (S_ISDIR(sb.st_mode) != S_ISDIR(tb.st_mode)) {
    KLOG_ERROR(LOG_TAG, "Could not bind-mount %s to %s: types do not match "
               "(%d != %d).\n", source, target, sb.st_mode, tb.st_mode);
    return "Could not match source and target bind types.";
  }

  if (mount(source, target, NULL, MS_BIND, NULL) < 0) {
    KLOG_ERROR(LOG_TAG, "Could not bind %s to %s: %s\n",
               source, target, strerror(errno));
    return "Could not bind item.";
  }

  KLOG_INFO(LOG_TAG, "Bound %s -> %s\n", source, target);

  return NULL;
}

const char* Container::ApplyCustomInit() {
  const char* custom_init_file = reader_->GetValueForKey(
      GceMetadataAttributes::kCustomInitFileKey);
  if (!custom_init_file) {
    custom_init_file = "";
  }

  AutoCloseFileDescriptor init_fd(open(
      kCustomInitFileName, O_CREAT|O_TRUNC|O_WRONLY, 0650));

  if (init_fd.IsError()) {
    KLOG_ERROR(LOG_TAG, "Could not create custom init file %s: %d (%s).\n",
               kCustomInitFileName, errno, strerror(errno));
  } else {
    size_t sz = strlen(custom_init_file);
    ssize_t written = TEMP_FAILURE_RETRY(write(init_fd, custom_init_file, sz));

    if (written == -1) {
      KLOG_WARNING(LOG_TAG,
                   "Warning: write failed on %s\n",
                   kCustomInitFileName);
    } else if (static_cast<size_t>(written) != sz) {
      KLOG_WARNING(LOG_TAG,
                   "Warning: short write to %s, wanted %zu, got %zu (%s)\n",
                   kCustomInitFileName, sz, written, strerror(errno));
    } else {
      KLOG_INFO(LOG_TAG, "Custom init file created. Wrote %zu bytes to %s.\n",
                written, kCustomInitFileName);
    }
  }
  return NULL;
}

const char* Container::ApplyMetadataProperties() {
  avd::DisplayProperties display;
  const char* metadata_value = reader_->GetValueForKey(
      GceMetadataAttributes::kDisplayConfigurationKey);
  display.Parse(metadata_value);
  if (!metadata_value) {
    KLOG_ERROR(LOG_TAG,
               "No display configuration specified. Using defaults.\n");
  } else if (display.IsDefault()) {
    KLOG_ERROR(LOG_TAG, "Bad display value ignored (%s). Using default.\n",
               metadata_value);
  }
  AutoFreeBuffer metadata_properties;
  metadata_properties.PrintF(
      "on early-init\n"
      "  setprop ro.sf.lcd_density %d\n"
      "  setprop ro.hw.headless.display %s\n",
      display.GetDpi(),
      display.GetConfig());
  AutoCloseFileDescriptor init_fd(open(
      kMetadataPropertiesFileName, O_CREAT|O_TRUNC|O_WRONLY, 0650));

  if (init_fd.IsError()) {
    KLOG_ERROR(LOG_TAG,
               "Could not create metadata properties file %s: %d (%s).\n",
               kMetadataPropertiesFileName, errno, strerror(errno));
  } else {
    ssize_t written = TEMP_FAILURE_RETRY(write(
        init_fd, metadata_properties.data(), metadata_properties.size()));
    if (static_cast<size_t>(written) != metadata_properties.size()) {
      KLOG_WARNING(LOG_TAG,
                   "Warning: short write to %s, wanted %zu, got %zu (%s)\n",
                   kMetadataPropertiesFileName, metadata_properties.size(),
                   written, strerror(errno));
    } else {
      KLOG_INFO(LOG_TAG,
                "Metadata properties created. Wrote %zu bytes to %s.\n",
                written, kMetadataPropertiesFileName);
    }
  }
  return NULL;
}

const char* Container::ApplyCustomization() {
  const char* res;

  // Check if we're booting mobile device. If so - initialize mobile network.
  std::map<std::string, std::string> target_properties;
  if (!avd::LoadPropertyFile(
      "/target/system/build.prop", &target_properties)) {
    return "Failed to load property file /target/system/build.prop.";
  }

  const std::string& rild_path = target_properties["rild.libpath"];
  if (rild_path.empty()) {
    device_type_ = kDeviceTypeWifi;
  } else {
    device_type_ = kDeviceType3G;
  }

  // Create custom init.rc file from metadata.
  res = ApplyCustomInit();
  if (res) return res;

  // Create properties based on the metadata.
  res = ApplyMetadataProperties();
  if (res) return res;

  res = BindFiles();
  if (res) return res;

  if (device_type_ == kDeviceType3G) {
    res = ConfigureNetworkMobile();
    if (res) return res;
  }

  // Run gce_mount_hander before switching /system partitions.
  // The properties setup will no longer be valid after the switch, and that
  // can cause libc crashes on KitKat.
  // We can't link this code in here because it depends on libext4_utils, and
  // we can't have shared library dependencies in /init.
  if (!is_nested_vm_) {
    KLOG_INFO(LOG_TAG, "Launching mount handler...\n");
    if (TEMP_FAILURE_RETRY(system("/system/bin/gce_mount_handler")) == -1) {
      KLOG_ERROR(LOG_TAG, "gce_mount_handler failed: %s\n", strerror(errno));
      return "Could not start gce_mount_handler.";
    }
  } else {
    // TODO(ender): we should be able to merge gce_mount_handler with gce_init
    // shortly. Make sure that while using nested virtualization we do launch
    // gce_mount_handler, too.
    avd::SharedFD file(avd::SharedFD::Open(
        "/target/fstab.gce_x86", O_RDWR | O_CREAT, 0640));
    const char fstab_data[] =
        "/dev/block/vdc /data ext4 nodev,noatime,nosuid,errors=panic wait\n";
    const char fstab_cache[] =
        "/dev/block/vdd /cache ext4 nodev,noatime,nosuid,errors=panic wait\n";

    file->Write(fstab_data, sizeof(fstab_data) - 1);
    file->Write(fstab_cache, sizeof(fstab_cache) - 1);

    avd::SharedFD ts_empty(
        avd::SharedFD::Open("/target/ts_snap.txt", O_RDWR | O_CREAT, 0444));
  }

  CopyFile("/initial.metadata", "/target/initial.metadata");

  return NULL;
}

const char* Container::CleanUp() {
  chdir("/target");

  // New filesystem does not have critical folders initialized.
  // Only bind-mount them here for the sake of mount_handler.
  // Shouldn't be needed once the mount handler is integrated in init.
  // "Hello, Container!"
  if (mount("/var", "/target/var", NULL, MS_MOVE, NULL))
    return "Could not bind /var.";

  // Unmount everything.
  if (umount("/system"))
    return "Could not unmount /system.";
  if (umount2("/proc", MNT_DETACH))
    return "Could not unmount /proc.";
  if (umount2("/sys", MNT_DETACH))
    return "Could not unmount /sys.";
  if (umount2("/dev", MNT_DETACH))
    return "Could not unmount /dev.";

  // Abandon current root folder.
  // If we don't do it, we won't be able to re-mount root.
  if (mount(".", "/", NULL, MS_MOVE, NULL))
    return "Could not move /.";
  chroot(".");

  // Do not execute anything here any more.
  // Environment is empty and must be initialized by android's init process.
  // Set any open file descriptors to close on exec.
  for (int i = 3; i < 1024; ++i) {
    fcntl(i, F_SETFD, FD_CLOEXEC);
  }

  return NULL;
}

const char* Container::PivotToNamespace(const char* name) {
  if (!ns_manager_->SwitchNamespace(name))
    return "Could not pivot to a different namespace.";

  return NULL;
}

int main() {
  klog_set_level(KLOG_INFO_LEVEL);
  Container container;
  AutoFreeBuffer reason;

  if (!Init(&container, &reason)) {
    KLOG_ERROR(LOG_TAG, "VIRTUAL_DEVICE_BOOT_FAILED : %s\n", reason.data());
    // There's no way of telling whether the problem happened before or after
    // /dev/kmsg became available. It's best to print out the log back to
    // console.
    printf("VIRTUAL_DEVICE_BOOT_FAILED : %s\n", reason.data());

    // If for some reason, however, Init completes, launch an emergency shell to
    // allow diagnosing what happened.
    system(kEmergencyShell);
    pause();
  }
}