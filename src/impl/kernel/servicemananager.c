static void service_run_and_wait(service_entry_t* svc) {
    process_t* proc = NULL;
    char target_path[MINIMAFS_MAX_PATH];
    const char* raw_path = NULL;

    if (svc->has_exec) {
        raw_path = svc->exec_path;
    } else if (svc->has_script) {
        char run_arg[MINIMAFS_MAX_PATH];
        if (service_script_run_path(svc->startup_script, run_arg, sizeof(run_arg))) {
            char normalized[MINIMAFS_MAX_PATH];
            const char* resolved = run_normalize_path(run_arg, normalized, sizeof(normalized));
            if (resolved) {
                strncpy(target_path, resolved, sizeof(target_path) - 1);
                target_path[sizeof(target_path) - 1] = '\0';
                raw_path = target_path;
            }
        } else {
            serial_write_str("[SERVICES] '");
            serial_write_str(service_display_name(svc));
            serial_write_str("': script is not a 'run <path>' command, "
                              "running once with no exit tracking\n");
            command_execute(svc->startup_script);
            return;
        }
    }

    if (raw_path) {
        char normalized[MINIMAFS_MAX_PATH];
        const char* resolved = run_normalize_path(raw_path, normalized, sizeof(normalized));
        if (resolved) {
            // Services launched this way take no arguments today.
            proc = run_launch_file(resolved, 0, NULL);
        }
    }

    if (!proc) {
        serial_write_str("[SERVICES] '");
        serial_write_str(service_display_name(svc));
        serial_write_str("': failed to start\n");
        return;
    }

    serial_write_str("[SERVICES] '");
    serial_write_str(service_display_name(svc));
    serial_write_str("' started (PID ");
    serial_write_dec(proc->pid);
    serial_write_str(")\n");

    thread_join(proc->pid);

    serial_write_str("[SERVICES] '");
    serial_write_str(service_display_name(svc));
    serial_write_str("' exited\n");
}
