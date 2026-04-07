// Standalone test to debug sd_bus_message seal/read in Docker
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <systemd/sd-bus.h>

int main()
{
    sd_bus* bus = NULL;
    int r = sd_bus_open_user(&bus);
    printf("sd_bus_open_user: %d\n", r);
    if (r < 0)
    {
        printf("  user bus failed: %s, trying system...\n", strerror(-r));
        r = sd_bus_open_system(&bus);
        printf("sd_bus_open_system: %d\n", r);
        if (r < 0)
        {
            printf("  system also failed: %s, trying sd_bus_open...\n",
                   strerror(-r));
            r = sd_bus_open(&bus);
            printf("sd_bus_open: %d\n", r);
            if (r < 0)
            {
                printf("  all failed: %s\n", strerror(-r));
                // Try creating a private bus just for message construction
                r = sd_bus_new(&bus);
                printf("sd_bus_new: %d\n", r);
                if (r < 0)
                    return 1;
                // Don't need to start it for message creation
            }
        }
    }

    // Create a method call message
    sd_bus_message* m = NULL;
    r = sd_bus_message_new_method_call(bus, &m, "xyz.openbmc_project.NSM",
                                       "/xyz/openbmc_project/NSM",
                                       "com.nvidia.DebugToken", "EraseToken");
    printf("new_method_call: %d, m=%p\n", r, (void*)m);
    if (r < 0 || !m)
    {
        printf("  error: %s\n", strerror(-r));
        sd_bus_unref(bus);
        return 1;
    }

    // === Test 1: Object path using method call message (as container) ===
    printf("\n=== Test 1: Object path ===\n");
    sd_bus_message* reply1 = NULL;
    // Create a method call just for data transport
    r = sd_bus_message_new_method_call(bus, &reply1, "a.b", "/a", "a.b", "M");
    printf("new_method_call (as reply): %d, reply=%p\n", r, (void*)reply1);
    if (reply1)
    {
        const char* path = "/com/nvidia/nsmd/async/1";
        r = sd_bus_message_append(reply1, "o", path);
        printf("append('o'): %d\n", r);
        r = sd_bus_message_seal(reply1, 0, 0);
        printf("seal: %d (%s)\n", r, r < 0 ? strerror(-r) : "ok");
        if (r >= 0)
        {
            const char* rpath = NULL;
            r = sd_bus_message_read(reply1, "o", &rpath);
            printf("read('o'): %d, path=%s\n", r, rpath ? rpath : "(null)");
        }
        sd_bus_message_unref(reply1);
    }

    // === Test 2: variant<string> using method call as container ===
    printf("\n=== Test 2: variant<string> ===\n");
    sd_bus_message* reply2 = NULL;
    r = sd_bus_message_new_method_call(bus, &reply2, "a.b", "/a", "a.b", "M2");
    printf("new_method_call: %d\n", r);
    if (reply2)
    {
        r = sd_bus_message_append(reply2, "v", "s", "Manual");
        printf("append('v','s','Manual'): %d\n", r);
        r = sd_bus_message_seal(reply2, 0, 0);
        printf("seal: %d (%s)\n", r, r < 0 ? strerror(-r) : "ok");
        if (r >= 0)
        {
            const char* rval = NULL;
            r = sd_bus_message_read(reply2, "v", "s", &rval);
            printf("read('v'): %d, val=%s\n", r, rval ? rval : "(null)");
        }
        sd_bus_message_unref(reply2);
    }

    // === Test 3: a{sa{sas}} (GetSubTreeResponse) ===
    printf("\n=== Test 3: a{sa{sas}} ===\n");
    sd_bus_message* reply3 = NULL;
    r = sd_bus_message_new_method_call(bus, &reply3, "a.b", "/a", "a.b", "M3");
    if (reply3)
    {
        r = sd_bus_message_open_container(reply3, 'a', "{sa{sas}}");
        printf("open 'a{sa{sas}}': %d\n", r);
        r = sd_bus_message_open_container(reply3, 'e', "sa{sas}");
        printf("open 'e': %d\n", r);
        const char* objpath = "/xyz/openbmc_project/NSM/gpu0";
        r = sd_bus_message_append_basic(reply3, 's', &objpath);
        printf("append path: %d\n", r);
        r = sd_bus_message_open_container(reply3, 'a', "{sas}");
        printf("open inner 'a': %d\n", r);
        r = sd_bus_message_open_container(reply3, 'e', "sas");
        printf("open inner 'e': %d\n", r);
        const char* svc = "xyz.openbmc_project.NSM";
        r = sd_bus_message_append_basic(reply3, 's', &svc);
        printf("append svc: %d\n", r);
        r = sd_bus_message_open_container(reply3, 'a', "s");
        printf("open iface 'a': %d\n", r);
        const char* iface = "com.nvidia.DebugToken";
        r = sd_bus_message_append_basic(reply3, 's', &iface);
        printf("append iface: %d\n", r);
        sd_bus_message_close_container(reply3); // a s
        sd_bus_message_close_container(reply3); // e
        sd_bus_message_close_container(reply3); // a{sas}
        sd_bus_message_close_container(reply3); // e
        sd_bus_message_close_container(reply3); // a{sa{sas}}
        r = sd_bus_message_seal(reply3, 0, 0);
        printf("seal: %d (%s)\n", r, r < 0 ? strerror(-r) : "ok");

        if (r >= 0)
        {
            // Try reading back
            r = sd_bus_message_enter_container(reply3, 'a', "{sa{sas}}");
            printf("read enter 'a': %d\n", r);
            r = sd_bus_message_at_end(reply3, 0);
            printf("at_end: %d (0=has data)\n", r);
            if (r == 0)
            {
                r = sd_bus_message_enter_container(reply3, 'e', "sa{sas}");
                printf("read enter 'e': %d\n", r);
                const char* rpath = NULL;
                r = sd_bus_message_read_basic(reply3, 's', &rpath);
                printf("read path: %d, val=%s\n", r, rpath ? rpath : "(null)");
            }
        }
        sd_bus_message_unref(reply3);
    }

    sd_bus_message_unref(m);
    sd_bus_unref(bus);
    printf("\nDone.\n");
    return 0;
}
