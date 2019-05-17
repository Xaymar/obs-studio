"use strict";

const process = require('process');
const runner = require('./runner.js');

// Steps
let configure_runners = [];
let build_runners = [];
let package_runners = [];

{
	let cmake_configure_extra = [
		'-DCPACK_GENERATOR="7Z"',
		'-DCPACK_SOURCE_GENERATOR="7Z"',
		`${process.env.CMAKE_EXTRA_FLAGS}`,
		`-DOBS_VERSION_OVERRIDE="${process.env.VERSION_OVERRIDE}"`
	];
	let cmake_build_extra = [
	];

	// Configuration depends on platform
	if (process.platform == "win32" || process.platform == "win64") {
		configure_runners.push(new runner('32-bit', 'cmake', [
				'-H.',
				'-Bbuild32',
				`-G"${process.env.CMAKE_GENERATOR}"`,
				`-DCMAKE_SYSTEM_VERSION=${process.env.CMAKE_SYSTEM_VERSION}`,
				`-DDepsPath="${process.cwd()}/cmbuild/win32"`,
				`-DQTDIR="${process.env.QT_PATH}"`,
				`-DCOPIED_DEPENDENCIES=false`,
			].concat(cmake_configure_extra)));
		configure_runners.push(new runner('64-bit', 'cmake', [
				'-H.',
				'-Bbuild64',
				`-G"${process.env.CMAKE_GENERATOR} Win64"`,
				'-T"host=x64"',
				`-DCMAKE_SYSTEM_VERSION=${process.env.CMAKE_SYSTEM_VERSION}`,
				`-DDepsPath="${process.cwd()}/cmbuild/win64"`,
				`-DQTDIR="${process.env.QT_PATH}_64"`,
				`-DCOPIED_DEPENDENCIES=false`,
			].concat(cmake_configure_extra)));
		
		// Extra build steps for AppVeyor on Windows for Logging purposes.
		if(process.env.APPVEYOR) {
			cmake_build_extra.concat(['--', '/logger:"C:\\Program Files\\AppVeyor\\BuildAgent\\Appveyor.MSBuildLogger.dll"']);
		}
	} else if (process.platform == "linux") {
		configure_runners.push(new runner('32-bit', 'cmake', [
				'-H.',
				'-Bbuild32',
				`-G"Unix Makefiles"`,
				`-DCOPIED_DEPENDENCIES=false`,
			].concat(cmake_configure_extra),
			{ ...process.env, ...{
				CFLAGS: `${process.env.COMPILER_FLAGS_32}`,
				CXXFLAGS: `${process.env.COMPILER_FLAGS_32}`,
			}}));
		configure_runners.push(new runner('64-bit', 'cmake', [
				'-H.',
				'-Bbuild64',
				`-G"Unix Makefiles"`,
				`-DCOPIED_DEPENDENCIES=false`,
			].concat(cmake_configure_extra),
			{ ...process.env, ...{
				CFLAGS: `${process.env.COMPILER_FLAGS_64}`,
				CXXFLAGS: `${process.env.COMPILER_FLAGS_64}`,
			}}));
	}
	
	build_runners.push(new runner('32-bit', 'cmake', [
			'--build', 'build32', 
			'--config', 'RelWithDebInfo'
		].concat(cmake_build_extra)));
	build_runners.push(new runner('64-bit', 'cmake', [
			'--build', 'build64', 
			'--config', 'RelWithDebInfo'
		].concat(cmake_build_extra)));
	package_runners.push(new runner('32-bit', 'cmake', [
			'--build', 'build32',
			'--target', 'PACKAGE',
			'--config', 'RelWithDebInfo'
		].concat(cmake_build_extra)));
	package_runners.push(new runner('64-bit', 'cmake', [
			'--build', 'build64',
			'--target', 'PACKAGE',
			'--config', 'RelWithDebInfo'
		].concat(cmake_build_extra)));
}

// Run Configure steps.
let configure_promises = [];
for (let config of configure_runners) {
	configure_promises.push(config.run());
}
Promise.all(configure_promises).then(function(result) {    
	let build_promises = [];
	for (let build of build_runners) {
		build_promises.push(build.run());
	}
	Promise.all(build_promises).then(function(result) {    
		let package_promises = [];
		for (let pack of package_runners) {
			package_promises.push(pack.run());
		}
		Promise.all(package_promises).then(function(result) {    
			process.exit(result);
		}).catch(function(result) {
			process.exit(result);
		});
	}).catch(function(result) {
		process.exit(result);
	});
}).catch(function(result) {
	process.exit(result);
});
