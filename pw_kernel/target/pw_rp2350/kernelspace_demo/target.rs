// Copyright 2025 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#![no_std]
#![no_main]

use arch_arm_cortex_m::Arch;
use target_common::{TargetInterface, declare_target};
use {console_backend as _, entry as _};

pub struct Target {}

impl TargetInterface for Target {
    const NAME: &'static str = "PW RP2350 Kernelspace Demo";

    fn console_init() {
        console_backend::init();
    }

    fn main() -> ! {
        static mut DEMO_STATE: demo::DemoState<Arch> = demo::DemoState::new(Arch);
        // SAFETY: `main` is only executed once, so we never generate more
        // than one `&mut` reference to `DEMO_STATE`.
        #[allow(static_mut_refs)]
        let _ = demo::main(Arch, unsafe { &mut DEMO_STATE });
        loop {}
    }
}

declare_target!(Target);
