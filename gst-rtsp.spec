%define         gst_majorminor 1.0

Name:           gstreamer-rtsp-server
Version:        1.8.3
Release:        1%{?dist}
Summary:        GStreamer based RTSP server
Vendor:		Collabora Multimedia
Group:          Applications/Multimedia
License:        LGPLv2+
Source0:        gst-rtsp-%{version}.tar.bz2
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires:  gstreamer-devel >= 0.10.11


%description
This is a RTSP server using the GStreamer framework.


%prep
%setup -q -n gst-rtsp-%{version}


%build
%configure

make

%install
%makeinstall

# Clean out files that should not be part of the rpm.
rm -f $RPM_BUILD_ROOT%{_libdir}/gstreamer-%{majorminor}/*.a
rm -f $RPM_BUILD_ROOT%{_libdir}/*.a
rm -f $RPM_BUILD_ROOT%{_libdir}/*.la

%clean
rm -rf $RPM_BUILD_ROOT


%post -p /sbin/ldconfig


%postun -p /sbin/ldconfig


%files
%defattr(-,root,root,-)
%doc AUTHORS COPYING README INSTALL docs/design/gst-rtp-server-design
%{_bindir}/gst-rtsp-server

%changelog
* Thu Oct 9 2008 Christian Schaller <christian.schaller@collabora.co.uk>
- First spec file
