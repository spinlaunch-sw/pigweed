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

use arch_riscv::Arch;
use pw_status::Result;
use riscv_semihosting::debug::{EXIT_FAILURE, EXIT_SUCCESS, exit};
use target_common::{TargetInterface, declare_target};
use {codegen as _, console_backend as _, entry as _};

pub struct Target {}
struct TargetUart {}

impl interrupts::TestUart for TargetUart {
    fn enable_loopback() {
        entry::UART0.enable_loopback()
    }

    fn read() -> Result<Option<u8>> {
        entry::UART0.read(Arch)
    }

    fn write(byte: u8) -> Result<()> {
        entry::UART0.write(byte)
    }
}

impl TargetInterface for Target {
    const NAME: &'static str = "QEMU-VIRT-RISCV Interrupts";

    fn main() -> ! {
        let exit_status = match { interrupts::main::<Arch, TargetUart>(Arch) } {
            Ok(()) => EXIT_SUCCESS,
            Err(_e) => EXIT_FAILURE,
        };
        exit(exit_status);
        loop {}
    }
}

declare_target!(Target);
