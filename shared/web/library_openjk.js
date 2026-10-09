// OpenJK's JavaScript library for the web build (emcc --js-library).
// Settings like JSPI are available here as preprocessor macros.

addToLibrary({
	// Whether the engine can wait for the browser to show a frame
	// (only when linked with -sJSPI, see Sys_WebWaitForFrame)
	ojk_can_wait_for_frame: () => {
#if JSPI
		return 1;
#else
		return 0;
#endif
	},

	ojk_wait_for_frame__async: true,
	ojk_wait_for_frame: () => {
#if JSPI
		return new Promise((resolve) => requestAnimationFrame(() => resolve()));
#endif
	},
});
