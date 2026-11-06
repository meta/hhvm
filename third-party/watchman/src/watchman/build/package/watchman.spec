Name: watchman
Version: %{version}
Release: 1%{?dist}
License: MIT
Summary: A file watching service
URL: https://meta.github.io/watchman/

%description

A file watching service.

%build

%install
mkdir -p %{buildroot}%{prefix}
cp -rvp %{image}/* %{buildroot}%{prefix}

%files
%defattr(-, root, root, -)
%{prefix}bin/watchman
%{prefix}bin/watchmanctl

%post

mkdir -p %{prefix}/var/run/watchman
chmod 2777 %{prefix}/var/run/watchman
