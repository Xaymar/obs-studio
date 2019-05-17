IF EXIST dependencies2017.zip (
	curl -kL "https://cdn-fastly.obsproject.com/downloads/dependencies2017.zip" -f --retry 5 -o dependencies2017.zip -z dependencies2017.zip
) ELSE (
	curl -kL "https://cdn-fastly.obsproject.com/downloads/dependencies2017.zip" -f --retry 5 -o dependencies2017.zip
)
7z x dependencies2017.zip -ocmbuild/deps

IF EXIST cef_binary_%CEF_VERSION%_windows32.zip (
	curl -kLO https://cdn-fastly.obsproject.com/downloads/cef_binary_%CEF_VERSION%_windows32.zip -f --retry 5 -z cef_binary_%CEF_VERSION%_windows32.zip
) ELSE (
	curl -kLO https://cdn-fastly.obsproject.com/downloads/cef_binary_%CEF_VERSION%_windows32.zip -f --retry 5 -C -
)
7z x cef_binary_%CEF_VERSION%_windows32.zip -ocmbuild/cef32

IF EXIST cef_binary_%CEF_VERSION%_windows64.zip (
	curl -kLO https://cdn-fastly.obsproject.com/downloads/cef_binary_%CEF_VERSION%_windows64.zip -f --retry 5 -z cef_binary_%CEF_VERSION%_windows64.zip
) ELSE (
	curl -kLO https://cdn-fastly.obsproject.com/downloads/cef_binary_%CEF_VERSION%_windows64.zip -f --retry 5 -C -
)
7z x cef_binary_%CEF_VERSION%_windows64.zip -ocmbuild/cef64

IF EXIST vlc.zip (
	curl -kLO https://cdn-fastly.obsproject.com/downloads/vlc.zip -f --retry 5 -z vlc.zip
) ELSE (
	curl -kLO https://cdn-fastly.obsproject.com/downloads/vlc.zip -f --retry 5 -C -
)
7z x vlc.zip -ocmbuild/vlc

SET "DepsPath32=%CD%\cmbuild\deps\win32"
SET "DepsPath64=%CD%\cmbuild\deps\win64"
SET "VLCPath=%CD%\cmbuild\vlc"
SET "QTDIR32=C:\Qt\5.13.0\msvc2017"
SET "QTDIR64=C:\Qt\5.13.0\msvc2017_64"
SET "CEF_32=%CD%\cef32\cef_binary_%CEF_VERSION%_windows32"
SET "CEF_64=%CD%\cef64\cef_binary_%CEF_VERSION%_windows64"
