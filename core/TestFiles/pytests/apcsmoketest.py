# apc_smoke_and_functionality_test.py
# Run this from the same environment as your pybind module.
import time, traceback, sys, struct, ctypes, platform
try:
    import sys
    import numpy as np
    sys.path.insert(0, "C:/PAPER/LCIM-BitTheorium/build")   # allow importing from build dir
    import atomiccim_bind as acb
except Exception as e:
    print("ERROR: import failed:", e)
    raise

def dbg(msg):
    print(f"[DBG {time.time():.6f}] {msg}", flush=True)

def find_working_nodes(container, cap=8, max_nodes_to_try=8):
    """Try InitOwned on a sequence of node ids to see which succeed.
       Returns (working_nodes, failing_nodes_with_excs).
    """
    working = []
    failing = {}
    for node in range(max_nodes_to_try):
        # keep attempts tiny to avoid big allocations
        try:
            cfg = acb.ContainerConf()
            cfg.ProducerBlockSize = 2
            cfg.BackgroundEpochAdvanceMS = 10
            cont = acb.AdaptivePackedCellContainer()
            dbg(f"Trying node {node} ...")
            # use the overload with alignment param
            try:
                cont.InitOwnedContainer(cap, node, cfg, 64)
                working.append(node)
                dbg(f"node {node} -> OK")
                # free quickly
                cont.FreeContainer()
            except MemoryError as me:
                dbg(f"node {node} -> MemoryError: {me}")
                failing[node] = ("MemoryError", str(me))
            except Exception as ex:
                dbg(f"node {node} -> Exception: {ex}")
                failing[node] = ("Other", str(ex))
        except Exception as outer:
            dbg(f"Unexpected top-level error testing node {node}: {outer}")
            failing[node] = ("Outer", str(outer))
    return working, failing

def smoke_init_with_fallbacks():
    dbg("Environment info:")
    dbg(f" platform.system(): {platform.system()}")
    dbg(f" platform.machine(): {platform.machine()}")
    dbg(f" python pointer size bits: {struct.calcsize('P')*8}")

    CAP = 64
    cfg = acb.ContainerConf()
    cfg.ProducerBlockSize = 8
    cfg.InitialMode = acb.PackedMode.MODE_VALUE32
    cfg.BackgroundEpochAdvanceMS = 25
    cfg.RetireBatchThreshold = 4

    attempts = [
        {"capacity": CAP, "node": 1, "alignment": 64},
        {"capacity": CAP, "node": 1, "alignment": None},
        {"capacity": CAP, "node": 0, "alignment": 64},
        {"capacity": CAP, "node": 0, "alignment": None},
        {"capacity": 32, "node": 0, "alignment": 64},
        {"capacity": 8, "node": 0, "alignment": 64},
    ]

    cont = acb.AdaptivePackedCellContainer()
    for i, a in enumerate(attempts, start=1):
        cap = a["capacity"]; node = a["node"]; alignment = a["alignment"]
        dbg(f"TRY #{i}: InitOwnedContainer(capacity={cap}, node={node}, alignment={alignment})")
        approx_bytes = int(cap) * 16
        try:
            _ = ctypes.create_string_buffer(approx_bytes)
            dbg("ctypes allocation OK (sanity check)")
        except Exception as e:
            dbg(f"ctypes allocation failed: {e}")

        try:
            if alignment is None:
                cont.InitOwnedContainer(cap, node, cfg)
            else:
                cont.InitOwnedContainer(cap, node, cfg, alignment)
            dbg(f"Init result: success True msg='Init success with capacity {cap} node {node} alignment {alignment}'")
            return cont, (cap, node, alignment)
        except MemoryError as me:
            dbg(f"C++ threw MemoryError for this variant: {me}")
        except Exception as ex:
            dbg(f"InitOwnedContainer raised exception (not MemoryError): {ex}")
            traceback.print_exc()

    raise RuntimeError("All init variants failed; see log above")

def full_functionality_test(cont):
    """After a successful InitOwnedContainer, exercise all public APIs."""
    dbg("Starting full functionality test")
    try:
        # 1) Reserve / NextProducerSequence
        base = cont.ReserveSlotsForProducer(4)
        dbg(f"ReserveSlotsForProducer(4) -> {base}")
        seq = cont.GetNextProducerSequence()
        dbg(f"GetNextProducerSequence() -> {seq}")

        # 2) Start/Stop background reclaimer
        dbg("Start background reclaimer")
        cont.StartBackgroungContainerReclaimer()
        time.sleep(0.05)
        dbg("Stop background reclaimer")
        cont.StopBackGroundContainerReclaimer()

        # 3) Publish pointer pair (unsafe address) and assemble
        sample_addr = 0x1234567890ABCDEF  # dummy pointer-sized integer
        dbg(f"PublishHeapPointerPairFrmAddress(addr=0x{sample_addr:x})")
        status, idx = cont.PublishHeapPointerPairFrmAddress(sample_addr, 0, -1)
        dbg(f"Publish -> (status={status}, idx={idx})")
        if status != acb.PublishStatus.OK:
            dbg("Publish did not succeed; container may be full or slots busy. Will try to probe further.")
        else:
            # Try assemble
            out = cont.TryAssemblePairedPtrFrmProbableIdx(idx)
            dbg(f"TryAssemblePairedPtrFrmProbableIdx({idx}) -> {out}")
            if out is not None:
                assembled_addr, relmode = out
                dbg(f"assembled_addr=0x{int(assembled_addr):x} relmode={relmode}")
            # retire it
            dbg(f"Retiring pair at probable idx {idx}")
            cont.RetirePtrPairFrmProbableIdx(idx)
            dbg("After retire -> call TryReclaimingRetiredContainer (may require epoch advance)")
            cont.TryReclaimingRetiredContainer()
            dbg("Try calling MenualAdvanceContainerEpoch(1) then TryReclaimingRetiredContainer again")
            cont.MenualAdvanceContainerEpoch(1)
            cont.TryReclaimingRetiredContainer()
            dbg("Poll device fences once:")
            pol = cont.PullContainerDeviceFencesJustOnce()
            dbg(f"PollDeviceFencesOnce_ -> {pol}")

        # 4) Region index init (if supported)
        try:
            dbg("Try InitContainerRegionOnIndex(4)")
            cont.InitContainerRegionOnIndex(4)
            dbg("InitContainerRegionOnIndex succeeded")
        except Exception as region_ex:
            dbg(f"InitContainerRegionOnIndex failed: {region_ex}")

        # 5) Fill the container until publish returns FULL (if possible)
        dbg("Attempting to publish repeatedly until FULL or until 2*capacity attempts")
        caps = 0
        max_tries = 2 * 1024
        publishes = 0
        full_seen = False
        for i in range(max_tries):
            status, idx = cont.PublishHeapPointerPairFrmAddress(0x1000 + i, 0, 0)
            publishes += 1
            if status == acb.PublishStatus.FULL:
                dbg(f"Publish returned FULL after {publishes} attempts")
                full_seen = True
                break
        if not full_seen:
            dbg(f"Did not observe FULL in {publishes} attempts (that's OK on some runs)")

        dbg("Functionality test finished OK")
    except Exception as e:
        dbg(f"full_functionality_test failed: {e}")
        traceback.print_exc()
        raise

def negative_tests():
    dbg("Running negative tests (expected failures)")
    # capacity 0 should throw; test wrapper should raise
    try:
        cont = acb.AdaptivePackedCellContainer()
        cfg = acb.ContainerConf()
        try:
            cont.InitOwnedContainer(0, acb.REL_NODE0, cfg, 64)
            dbg("ERROR: InitOwnedContainer(0, ...) unexpectedly succeeded")
        except Exception as e:
            dbg(f"InitOwnedContainer(0, ...) raised as expected: {e}")
    except Exception as outer:
        dbg(f"unexpected: {outer}")

if __name__ == "__main__":
    try:
        # quick node discovery
        dbg("Probing nodes 0..3 for allocator availability (small allocations)")
        nodes_ok, nodes_fail = find_working_nodes(None, cap=8, max_nodes_to_try=4)
        dbg(f"Working nodes: {nodes_ok}  Failures: {nodes_fail}")

        # run smoke with fallback sequence
        container, used = smoke_init_with_fallbacks()
        dbg(f"Container init OK used params: {used}")

        # run full functionality test (this will exercise public API)
        full_functionality_test(container)

        # negative tests
        negative_tests()

        dbg("Cleaning up container")
        try:
            container.StopBackGroundContainerReclaimer()
        except Exception:
            pass
        container.FreeContainer()
        dbg("Finished all tests successfully")

    except Exception as final:
        dbg(f"Test-run aborted: {final}")
        traceback.print_exc()
        sys.exit(2)