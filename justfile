# orouteragent - development and end-to-end test tasks
#
#   just              list the tasks
#   just check        host unit tests + fuzz pass
#   just build        cross-build the package for the real target
#   just e2e-up       bring up controller + OpenWrt VM and adopt

set shell := ["bash", "-uc"]

e2e := justfile_directory() / "e2e-testing"
compose := "docker compose -f " + e2e / "docker-compose.yml"
vm_ip := env_var_or_default("E2E_OPENWRT_IP", "172.28.0.50")
ssh_key := e2e / "openwrt/ssh/id_ed25519"
ssh_opts := "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i " + ssh_key

# Show the available tasks.
default:
    @just --list --unsorted

# ---------------------------------------------------------------- build

# Cross-build the package using the official OpenWrt SDK container.
# Pass a different target as needed, e.g. 'just build x86/64'.
build target="mediatek/filogic":
    TARGET={{target}} scripts/build-package.sh

# Static analysis of every source file with gcc -fanalyzer.
analyze target="mediatek/filogic":
    TARGET={{target}} scripts/analyze.sh

# Drop the cached SDK volumes (forces a clean SDK on the next build).
sdk-clean:
    #!/usr/bin/env bash
    set -euo pipefail
    vols=$(docker volume ls -q --filter name=orouteragent-sdk- || true)
    if [[ -z "$vols" ]]; then
        echo "no SDK volumes to remove"
    else
        echo "$vols" | xargs docker volume rm
    fi

# Run the host unit tests and the fuzz pass (ASan/UBSan).
check:
    make -C test check

# Longer fuzz campaign: just fuzz 200000
fuzz iters="200000" seed="0":
    make -C test fuzz FUZZ_ITERS={{iters}} FUZZ_SEED={{seed}}

# Remove build outputs.
clean:
    make -C test clean
    rm -f {{e2e}}/artifacts/orouteragent-*.apk

# ------------------------------------------------------------------ e2e

# Build the agent for x86-64 and stage it for the VM.
e2e-build:
    {{e2e}}/scripts/build-agent-x86.sh

# Create the ssh keypair the harness uses to reach the VM.
e2e-keygen:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -f {{ssh_key}} ]]; then
        echo "key already exists: {{ssh_key}}"
    else
        mkdir -p "$(dirname {{ssh_key}})"
        ssh-keygen -t ed25519 -N '' -C orouteragent-e2e -f {{ssh_key}}
        echo "created {{ssh_key}}"
    fi

# Start the whole lab (controller, proxy, OpenWrt VM) and wait for it.
e2e-up: e2e-keygen
    {{compose}} up -d --build
    @echo
    @echo "controller UI : https://127.0.0.1:8043  (or http://127.0.0.1:8090 via the proxy)"
    @echo "VM console    : just e2e-console"
    @echo
    {{e2e}}/scripts/wait-ready.sh

# Stop the lab but keep the controller database and the VM disk.
e2e-down:
    {{compose}} stop

# Stop the lab and delete all of its state (controller db, VM disk).
e2e-destroy:
    {{compose}} down -v

# Restart everything from scratch.
e2e-reset: e2e-destroy e2e-up

# Show container status.
e2e-status:
    {{compose}} ps
    @echo
    @curl -sk https://127.0.0.1:8043/api/info | head -c 400 || echo "controller not responding"

# Install the freshly built package into the running VM and restart the agent.
e2e-install:
    {{e2e}}/scripts/install-agent.sh

# Build, install and restart the agent in one step.
e2e-deploy: e2e-build e2e-install

# Follow the controller log.
e2e-controller-logs:
    {{compose}} logs --follow controller

# Follow the proxy log (WebSocket upgrades show up here).
e2e-proxy-logs:
    {{compose}} logs --follow proxy

# Follow DHCP activity from the simulated ISP gateway.
e2e-isp-logs:
    {{compose}} logs --follow isp-gateway

# Attach to the VM serial console (ctrl-] to leave).
e2e-console:
    @echo "connecting to the VM serial console; press ctrl-] to detach"
    telnet 127.0.0.1 2323

# Open a shell on the OpenWrt VM. Pass a command to run it remotely,
# e.g. 'just e2e-ssh logread | tail -20'.
e2e-ssh *args:
    ssh -t {{ssh_opts}} root@{{vm_ip}} {{quote(args)}}

# Follow the agent log on the VM (frames and redacted GET/SET data at level 3).
# Give a line count to print the tail without following, e.g. 'just e2e-agent-logs 50'.
e2e-agent-logs n="":
    ssh {{ssh_opts}} root@{{vm_ip}} \
        'logread -e orouteragent {{ if n != "" { "" } else { "-f" } }} \
         {{ if n != "" { "| tail -n " + n + " | cat" } else { "" } }}'

# Show what the agent is doing right now.
e2e-agent-status:
    ssh {{ssh_opts}} root@{{vm_ip}} \
        'echo "--- process ---"; pgrep -a orouteragentd || echo "not running"; \
         echo "--- config ---"; uci show orouteragent; \
         echo "--- state ---"; cat /etc/orouteragent/state.json 2>/dev/null || echo "(no state yet)"; \
         echo; echo "--- recent log ---"; logread -e orouteragent | tail -30'

# Restart the agent on the VM.
e2e-agent-restart:
    ssh {{ssh_opts}} root@{{vm_ip}} '/etc/init.d/orouteragent restart; sleep 1; pgrep -a orouteragentd'

# One-time controller wizard: needed once per lab before it will adopt.
e2e-controller-setup:
    #!/usr/bin/env bash
    set -euo pipefail
    info="$(curl -sk --max-time 10 https://127.0.0.1:8043/api/info || true)"
    if [[ "$info" == *'"configured":true'* ]]; then
        echo "controller is already set up - nothing to do"
        exit 0
    fi
    cat <<'EOS'
    The controller needs its setup wizard completed once. Its database lives
    in a docker volume, so this survives 'just e2e-down' and is only needed
    again after 'just e2e-destroy'.

      1. open http://127.0.0.1:8090          (the proxy; the direct
                                              https://127.0.0.1:8043 also works
                                              if you accept the self-signed cert)
      2. complete the wizard: controller name, admin user, and a site
    3. skip the cloud/"TP-L*nk ID" step - the lab works fully offline
      4. in the site, go to Devices; the agent shows up as "Pending"
         within ~10s and can be adopted

    Use the same admin credentials in the agent's device account if you
    change them from the defaults:
      uci set orouteragent.agent.device_username=<user>
      uci set orouteragent.agent.device_password=<pass>
    EOS

# List the devices the controller currently knows about.
e2e-devices:
    {{e2e}}/scripts/controller-devices.sh