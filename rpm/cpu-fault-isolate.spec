%define kmod_name cpu_fault_isolate

Name:           cpu-fault-isolate
Version:        0.1.0
Release:        1%{?dist}
Summary:        CPU Core/Cache Fault Isolation for improved system reliability
License:        GPL-2.0
Group:          System/Kernel
URL:            https://gitee.com/openeuler/cpu-fault-isolate
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  kernel-devel
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  sqlite-devel
BuildRequires:  libvirt-devel
BuildRequires:  systemd

Requires:       kernel >= 6.6
Requires:       sqlite
Requires:       libvirt-libs

%description
CPU Fault Isolation (CFI) provides automatic detection and isolation of
faulty CPU cores and caches on x86_64 and aarch64 (Kunpeng) systems.
When CPU hardware errors (cache faults, core faults) are detected, the
system automatically isolates the faulty core to prevent whole-machine
failure, while gracefully migrating VM workloads to healthy cores.

This package contains:
  - Kernel module (cpu_fault_isolate.ko)
  - Userspace daemon (cfid)
  - CLI tool (cfi-ctl)

%package -n kmod-%{kmod_name}
Summary:        CPU Fault Isolation kernel module
Group:          System/Kernel
Requires:       kernel >= 6.6

%description -n kmod-%{kmod_name}
Kernel module for CPU core/cache fault detection and isolation.

%prep
%setup -q

%build
# Build kernel module
make -C kernel KDIR=%{_usrsrc}/kernels/%{kernel_version}

# Build userspace components
make -C daemon
make -C cli

%install
# Kernel module
install -D -m 644 kernel/%{kmod_name}.ko \
    %{buildroot}/lib/modules/%{kernel_version}/extra/%{kmod_name}.ko

# Daemon and CLI
install -D -m 755 daemon/cfid %{buildroot}%{_sbindir}/cfid
install -D -m 755 cli/cfi-ctl %{buildroot}%{_bindir}/cfi-ctl

# Configuration
install -D -m 644 config/cfid.conf.example \
    %{buildroot}%{_sysconfdir}/cfid/cfid.conf
install -D -m 644 config/cfid.service \
    %{buildroot}%{_unitdir}/cfid.service

# Directories
install -d %{buildroot}%{_localstatedir}/lib/cfid
install -d %{buildroot}%{_localstatedir}/log/cfid

%post
/sbin/depmod -a
%systemd_post cfid.service

%preun
%systemd_preun cfid.service

%postun
/sbin/depmod -a
%systemd_postun_with_restart cfid.service

%files
%license LICENSE
%doc README.md docs/
%{_sbindir}/cfid
%{_bindir}/cfi-ctl
%config(noreplace) %{_sysconfdir}/cfid/cfid.conf
%{_unitdir}/cfid.service
%dir %{_localstatedir}/lib/cfid
%dir %{_localstatedir}/log/cfid

%files -n kmod-%{kmod_name}
/lib/modules/%{kernel_version}/extra/%{kmod_name}.ko

%changelog
* Thu May 15 2026 openEuler Community <dev@openeuler.org> - 0.1.0-1
- Initial release
- Kernel module with x86 MCA/MCE and ARM64 GHES/APEI backends
- Per-CPU error accounting with sliding window thresholds
- Automatic CPU isolation via hotplug
- Daemon-deferred isolation protocol for VM safety
- Sysfs and generic netlink interfaces
