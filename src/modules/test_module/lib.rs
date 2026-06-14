#![no_std]

#[repr(C)]
pub struct KernelApi {
    pub kmalloc: Option<extern "C" fn(usize) -> *mut core::ffi::c_void>,
    pub kfree: Option<extern "C" fn(*mut core::ffi::c_void)>,
    pub printf: Option<unsafe extern "C" fn(*const u8, ...)>,
}

#[no_mangle]
pub extern "C" fn rust_module_init(api: *const KernelApi) -> i32 {
    if api.is_null() {
        return -1;
    }
    let api = unsafe { &*api };
    if let Some(printf) = api.printf {
        let msg1 = b"Hello from Rust module!\n\0";
        let msg2 = b"Rust module: kernel API works! \n\0";
        unsafe {
            printf(msg1.as_ptr());
            printf(msg2.as_ptr());
        }
    }
    0
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
