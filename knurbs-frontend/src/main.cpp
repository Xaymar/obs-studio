// Knurbs frontend entry point.
//
// Only Bootstrap runs before QApplication exists. Everything else is Qt code
// that behaves the same on every platform.

#include "app/Application.hpp"
#include "app/Bootstrap.hpp"

int main(int argc, char *argv[])
{
	knurbs::bootstrap::harden_process();
	knurbs::bootstrap::install_crash_handler();
	knurbs::bootstrap::set_application_attributes();

	knurbs::Application app(argc, argv);
	return app.run();
}
