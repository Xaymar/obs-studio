#pragma once

// The few steps that must run before QApplication is constructed.
namespace knurbs::bootstrap {

// Windows: safe DLL search order and process mitigations. No-op elsewhere.
void harden_process();

// Installs libobs' crash handler so crashes during startup are logged.
void install_crash_handler();

// Qt attributes that must be set before QApplication exists.
void set_application_attributes();

} // namespace knurbs::bootstrap
