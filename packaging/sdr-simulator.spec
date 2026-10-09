# Version/release are injected by CI (see .github/workflows/rpm.yml); the defaults allow a
# plain local `rpmbuild -ba packaging/sdr-simulator.spec` with a matching source tarball.
%{!?pkg_version: %global pkg_version 0.1.0}
%{!?pkg_release: %global pkg_release 1}

Name:           sdr-simulator
Version:        %{pkg_version}
Release:        %{pkg_release}%{?dist}
Summary:        SDR receiver simulator streaming VITA-49 IQ data
License:        LicenseRef-Unspecified
URL:            https://github.com/HBrauer/simulator
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  python3
BuildRequires:  systemd-rpm-macros
BuildRequires:  pkgconfig(libmicrohttpd)
BuildRequires:  pkgconfig(jansson)
BuildRequires:  pkgconfig(yaml-0.1)
BuildRequires:  pkgconfig(check)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(fftw3f)
BuildRequires:  pkgconfig(volk)
Requires(pre):  shadow-utils
%{?systemd_requires}

%description
Simulates an SDR receiver: renders configurable scenarios (noise, tones, bursts, IQ and audio
replays) into wideband and DDC channels and streams them as VITA-49 over UDP, controlled via a
REST API.

Each simulator runs from a setup folder (receiver.yaml, scenario.yaml, assets/) that can live
anywhere. Create one with `sdr-simulator --init DIR`. Several simulators can run side by side as
systemd instances, sdr-simulator@NAME, each configured by /etc/sdr-simulator/instances/NAME.conf.
Run without a setup folder, it streams a built-in noise-only starter setup.

%prep
%autosetup

%build
# No -march=native: the package must run on any x86-64 machine.
%meson -Dnative_optimizations=false -Dvolk_accel=enabled -Dliquid_resampler=disabled
%meson_build

%install
%meson_install
install -Dpm 0644 packaging/sdr-simulator@.service %{buildroot}%{_unitdir}/sdr-simulator@.service
install -Dpm 0644 packaging/sdr-simulator.sysusers %{buildroot}%{_sysusersdir}/sdr-simulator.conf

%check
%meson_test --suite unit

%pre
# Fallback for rpm versions that do not apply sysusers.d files themselves.
getent group sdr-simulator >/dev/null || groupadd -r sdr-simulator
getent passwd sdr-simulator >/dev/null || \
    useradd -r -g sdr-simulator -d / -s /sbin/nologin -c "SDR receiver simulator" sdr-simulator
exit 0

%post
%systemd_post sdr-simulator@.service

%preun
%systemd_preun 'sdr-simulator@*.service'

%postun
%systemd_postun_with_restart 'sdr-simulator@*.service'

%files
%doc README.md docs/quick_start.md docs/configuration_manual.md docs/rest_api.md docs/vita49_udp.md
%{_bindir}/sdr-simulator
%{_unitdir}/sdr-simulator@.service
%{_sysusersdir}/sdr-simulator.conf

%changelog
* Thu Oct 08 2026 HBrauer <henrikbrauer@gmail.com> - 0.1.0-1
- Initial package
