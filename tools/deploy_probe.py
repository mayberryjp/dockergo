#!/usr/bin/env python3
"""Prototype the firmware's container-recreate flow against a Docker daemon.

Talks to the Engine API over plain HTTP (same as the firmware) and builds the
/containers/create body the same way docker_client.cpp does, so we can see the
exact error the daemon returns and iterate until create+start actually works.

Usage:
  python tools/deploy_probe.py list
  python tools/deploy_probe.py inspect <name|id>
  python tools/deploy_probe.py probe <name|id> [new_image]
  python tools/deploy_probe.py recreate <name|id> [new_image]
  python tools/deploy_probe.py deploy <image>   # grab image + recreate (full process)

`probe` never touches the live container: it builds the create body, creates a
throwaway "<name>_probe" container, starts it, reports its state, then removes
it. Iterate on build_create_body() here until it comes up cleanly, then port the
result back to captureContainer() in C++.
"""
import json
import sys
import time
import http.client

HOST = "10.2.10.2"
PORT = 2375


def api(method, path, body=None):
    conn = http.client.HTTPConnection(HOST, PORT, timeout=20)
    headers = {}
    payload = None
    if body is not None:
        payload = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    conn.request(method, path, payload, headers)
    resp = conn.getresponse()
    raw = resp.read()
    conn.close()
    data = None
    if raw:
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            data = raw.decode(errors="replace")
    return resp.status, data


def find_container(needle):
    status, arr = api("GET", "/containers/json?all=1")
    if status != 200:
        print(f"list HTTP {status}: {arr}")
        sys.exit(1)
    for c in arr:
        names = [n.lstrip("/") for n in c.get("Names", [])]
        if needle == c["Id"] or needle in c["Id"][:12] or needle in names \
                or needle == c.get("Image"):
            return c
    print(f"no container matching {needle!r}")
    sys.exit(1)


def is_auto_hostname(hn):
    return len(hn) == 12 and all(ch in "0123456789abcdef" for ch in hn)


def build_create_body(ins, new_image):
    """Mirror captureContainer() in docker_client.cpp."""
    cfg = ins["Config"]
    hcfg = ins["HostConfig"]

    body = {"Image": new_image}
    for k_body, k_cfg in [
        ("User", "User"), ("WorkingDir", "WorkingDir"), ("Env", "Env"),
        ("Cmd", "Cmd"), ("Entrypoint", "Entrypoint"), ("Labels", "Labels"),
        ("ExposedPorts", "ExposedPorts"), ("Volumes", "Volumes"),
    ]:
        v = cfg.get(k_cfg)
        if v:
            body[k_body] = v

    hn = cfg.get("Hostname", "")
    if hn and not is_auto_hostname(hn):
        body["Hostname"] = hn

    hc = {}
    for k in ["Binds", "Mounts", "PortBindings", "RestartPolicy", "NetworkMode",
              "CapAdd", "CapDrop", "SecurityOpt", "Devices", "ExtraHosts",
              "Dns", "DnsSearch", "VolumesFrom", "PidMode", "IpcMode", "Tmpfs"]:
        v = hcfg.get(k)
        if v:
            hc[k] = v
    if hcfg.get("Privileged"):
        hc["Privileged"] = True
    body["HostConfig"] = hc

    nets = ins.get("NetworkSettings", {}).get("Networks", {}) or {}
    first = True
    extra = []
    for name, src in nets.items():
        if first:
            ec = {}
            if src.get("Aliases"):
                ec["Aliases"] = src["Aliases"]
            if src.get("IPAMConfig"):
                ec["IPAMConfig"] = src["IPAMConfig"]
            body["NetworkingConfig"] = {"EndpointsConfig": {name: ec}}
            first = False
        else:
            extra.append((name, {"Aliases": src.get("Aliases")} if src.get("Aliases") else {}))
    return body, extra


def cmd_list():
    status, arr = api("GET", "/containers/json?all=1")
    for c in arr:
        names = ",".join(n.lstrip("/") for n in c.get("Names", []))
        print(f"{c['Id'][:12]}  {c['State']:9}  {names:30}  {c.get('Image')}")


def cmd_inspect(needle):
    c = find_container(needle)
    status, ins = api("GET", f"/containers/{c['Id']}/json")
    print(json.dumps({"Config": ins["Config"], "HostConfig": ins["HostConfig"],
                      "Networks": ins.get("NetworkSettings", {}).get("Networks")},
                     indent=2))


def cmd_probe(needle, new_image=None):
    c = find_container(needle)
    status, ins = api("GET", f"/containers/{c['Id']}/json")
    if status != 200:
        print(f"inspect HTTP {status}: {ins}")
        return
    image = new_image or c.get("Image")
    body, extra = build_create_body(ins, image)
    name = c["Names"][0].lstrip("/") + "_probe"

    print(f"== create body for {name} (image={image}) ==")
    print(json.dumps(body, indent=2))

    api("DELETE", f"/containers/{name}?force=1")  # clear a prior probe
    status, resp = api("POST", f"/containers/create?name={name}", body)
    print(f"\ncreate -> HTTP {status}: {resp}")
    if status not in (200, 201):
        return
    new_id = resp["Id"]

    for net, ep in extra:
        s, r = api("POST", f"/networks/{net}/connect",
                   {"Container": new_id, "EndpointConfig": ep})
        print(f"connect {net} -> HTTP {s}: {r}")

    status, resp = api("POST", f"/containers/{new_id}/start")
    print(f"start -> HTTP {status}: {resp}")

    time.sleep(2)
    status, ins2 = api("GET", f"/containers/{new_id}/json")
    state = ins2.get("State", {})
    print(f"state -> {state.get('Status')} exit={state.get('ExitCode')} err={state.get('Error')!r}")

    s, logs = api("GET", f"/containers/{new_id}/logs?stdout=1&stderr=1&tail=20")
    if isinstance(logs, str):
        print("logs:\n" + logs)

    print(f"\ncleaning up probe {name}")
    api("POST", f"/containers/{new_id}/stop?t=5")
    api("DELETE", f"/containers/{new_id}?force=1")


def pull_image(repo, tag):
    """Pull an image via the daemon (POST /images/create), streaming progress."""
    conn = http.client.HTTPConnection(HOST, PORT, timeout=600)
    conn.request("POST", f"/images/create?fromImage={repo}&tag={tag}")
    resp = conn.getresponse()
    if resp.status not in (200, 201):
        body = resp.read().decode(errors="replace")
        conn.close()
        return False, f"pull HTTP {resp.status}: {body}"
    last, buf = "", b""
    while True:
        chunk = resp.read(512)
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "error" in obj:
                conn.close()
                return False, obj["error"]
            status = obj.get("status", "")
            if status and status != last:
                last = status
                print(f"  pull: {status}")
    conn.close()
    return True, None


def cmd_deploy(image):
    """The full firmware process: grab (pull) the image, then recreate the
    matching running container on it, with rollback."""
    repo, tag = (image.rsplit(":", 1) + ["latest"])[:2]
    c = find_container(image)
    name = c["Names"][0].lstrip("/")

    _, before = api("GET", f"/containers/{c['Id']}/json")
    old_img_id = before.get("Image")
    print(f"target {name}  running image id {old_img_id}")

    print(f"\n== grab {image} ==")
    ok, err = pull_image(repo, tag)
    if not ok:
        print(f"pull failed: {err}")
        return
    status, imginfo = api("GET", f"/images/{image}/json")
    if status != 200:
        print(f"image not present after pull: HTTP {status}")
        return
    new_img_id = imginfo.get("Id")
    print(f"pulled image id {new_img_id}")
    if new_img_id == old_img_id:
        print("(image id unchanged — recreating on the same image)")

    print(f"\n== recreate {name} on {image} ==")
    cmd_recreate(name, image)

    status, after = api("GET", f"/containers/{name}/json")
    if status == 200:
        st = after.get("State", {})
        print(f"\nfinal: {name} image={after.get('Image')} status={st.get('Status')}")
        print("updated to new image OK" if after.get("Image") == new_img_id
              else "still on old image FAIL")


def cmd_recreate(needle, new_image=None):
    """Full firmware sequence on the live container, with rollback."""
    c = find_container(needle)
    status, ins = api("GET", f"/containers/{c['Id']}/json")
    image = new_image or c.get("Image")
    body, extra = build_create_body(ins, image)
    old_id = c["Id"]
    name = c["Names"][0].lstrip("/")
    backup = name + "_old"

    print(f"stopping {name}")
    print("  stop  ->", api("POST", f"/containers/{old_id}/stop?t=10")[0])
    api("DELETE", f"/containers/{backup}?force=1")
    rn = api("POST", f"/containers/{old_id}/rename?name={backup}")[0]
    print(f"  rename-> {rn}")
    if rn not in (200, 204):
        print("rename failed; restarting old")
        api("POST", f"/containers/{old_id}/start")
        return

    fail = None
    status, resp = api("POST", f"/containers/create?name={name}", body)
    print(f"  create-> HTTP {status}: {resp}")
    new_id = None
    if status in (200, 201):
        new_id = resp["Id"]
    else:
        fail = f"create HTTP {status}"

    if not fail:
        for net, ep in extra:
            print(f"  connect {net} ->", api("POST", f"/networks/{net}/connect",
                                             {"Container": new_id, "EndpointConfig": ep})[0])
        s = api("POST", f"/containers/{new_id}/start")[0]
        print(f"  start -> HTTP {s}")
        if s not in (200, 204):
            fail = f"start HTTP {s}"

    if not fail:
        time.sleep(2)
        st = api("GET", f"/containers/{new_id}/json")[1].get("State", {})
        print(f"  state -> {st.get('Status')} err={st.get('Error')!r}")
        if st.get("Status") != "running":
            fail = f"state {st.get('Status')}"

    if not fail:
        print(f"SUCCESS: {name} recreated; removing {backup}")
        api("DELETE", f"/containers/{old_id}?force=1")
    else:
        print(f"FAIL ({fail}); rolling back")
        if new_id:
            api("DELETE", f"/containers/{new_id}?force=1")
        api("POST", f"/containers/{old_id}/rename?name={name}")
        api("POST", f"/containers/{old_id}/start")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "list":
        cmd_list()
    elif cmd == "inspect" and len(sys.argv) >= 3:
        cmd_inspect(sys.argv[2])
    elif cmd == "probe" and len(sys.argv) >= 3:
        cmd_probe(sys.argv[2], sys.argv[3] if len(sys.argv) >= 4 else None)
    elif cmd == "recreate" and len(sys.argv) >= 3:
        cmd_recreate(sys.argv[2], sys.argv[3] if len(sys.argv) >= 4 else None)
    elif cmd == "deploy" and len(sys.argv) >= 3:
        cmd_deploy(sys.argv[2])
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
