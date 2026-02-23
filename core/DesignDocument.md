1. To handle exception where BlindModeSwitchOfPacked can have Exception MODE new LOCALITY->ST_EXCEPTION_BIT_FAULTY::Introduced.
2. 
|15      12|11 9|8|7   4|3 2|1 0|
|Priority   |Loc |T|  |RO |DT |

3. RelOffset(2bit):: Unsigned
        RelOffset semantics you 
        1 indicates “head pointing to tail” (head → tail).
        2 indicates “tail pointing to head”.
        0 indicates standalone (no pointer pairing).
        Consumers should treat only reloff == +1 or -1 as paired; anything else is standalone or ignored. -2 -> is reserved for now.



Current smoke test Result::
(venv) PS C:\PAPER\LCIM-BitTheorium\core\TestFiles\pytests> python apcsmoketest.py
[DBG 1771832180.729321] Probing nodes 0..3 for allocator availability (small allocations)
[DBG 1771832180.729791] Trying node 0 ...
[DBG 1771832180.729979] node 0 -> OK
[DBG 1771832180.730430] Trying node 1 ...
[DBG 1771832180.730620] node 1 -> MemoryError: bad allocation
[DBG 1771832180.730729] Trying node 2 ...
[DBG 1771832180.730798] node 2 -> MemoryError: bad allocation
[DBG 1771832180.730878] Trying node 3 ...
[DBG 1771832180.730943] node 3 -> MemoryError: bad allocation
[DBG 1771832180.730975] Working nodes: [0]  Failures: {1: ('MemoryError', 'bad allocation'), 2: ('MemoryError', 'bad allocation'), 3: ('MemoryError', 'bad allocation')}
[DBG 1771832180.730992] Environment info:
[DBG 1771832180.778989]  platform.system(): Windows
[DBG 1771832180.779052]  platform.machine(): AMD64
[DBG 1771832180.779084]  python pointer size bits: 64
[DBG 1771832180.779229] TRY #1: InitOwnedContainer(capacity=64, node=1, alignment=64)
[DBG 1771832180.779309] ctypes allocation OK (sanity check)
[DBG 1771832180.779422] C++ threw MemoryError for this variant: bad allocation
[DBG 1771832180.779446] TRY #2: InitOwnedContainer(capacity=64, node=1, alignment=None)
[DBG 1771832180.779468] ctypes allocation OK (sanity check)
[DBG 1771832180.779515] C++ threw MemoryError for this variant: bad allocation
[DBG 1771832180.779531] TRY #3: InitOwnedContainer(capacity=64, node=0, alignment=64)
[DBG 1771832180.779547] ctypes allocation OK (sanity check)
[DBG 1771832180.779648] Init result: success True msg='Init success with capacity 64 node 0 alignment 64'
[DBG 1771832180.779695] Container init OK used params: (64, 0, 64)
[DBG 1771832180.779724] Starting full functionality test
[DBG 1771832180.779765] ReserveSlotsForProducer(4) -> 0
[DBG 1771832180.779790] GetNextProducerSequence() -> 4
[DBG 1771832180.779805] Start background reclaimer
[DBG 1771832180.830276] Stop background reclaimer
[DBG 1771832180.830663] PublishHeapPointerPairFrmAddress(addr=0x1234567890abcdef)
[DBG 1771832180.830813] Publish -> (status=PublishStatus.OK, idx=5)
[DBG 1771832180.830948] TryAssemblePairedPtrFrmProbableIdx(5) -> (3617611475434851823, 2)
[DBG 1771832180.831015] assembled_addr=0x3234567850abcdef relmode=2
[DBG 1771832180.831095] Retiring pair at probable idx 5
[DBG 1771832180.831181] After retire -> call TryReclaimingRetiredContainer (may require epoch advance)
[DBG 1771832180.831215] Try calling MenualAdvanceContainerEpoch(1) then TryReclaimingRetiredContainer again
[DBG 1771832180.831254] Poll device fences once:
[DBG 1771832180.831287] PollDeviceFencesOnce_ -> False
[DBG 1771832180.831335] Try InitContainerRegionOnIndex(4)
[DBG 1771832180.831418] InitContainerRegionOnIndex succeeded
[DBG 1771832180.831463] Attempting to publish repeatedly until FULL or until 2*capacity attempts
[DBG 1771832180.831573] Publish returned FULL after 2 attempts
[DBG 1771832180.831602] Functionality test finished OK
[DBG 1771832180.831636] Running negative tests (expected failures)
[DBG 1771832180.831757] InitOwnedContainer(0, ...) raised as expected: Capacity == 0
[DBG 1771832180.831810] Cleaning up container
(venv) PS C:\PAPER\LCIM-BitTheorium\core\TestFiles\pytests> 

New smoke test result:
venv) PS C:\PAPER\LCIM-BitTheorium\core\TestFiles\pytests> python apcsmoketest.py
[DBG 1771833098.025719] Probing nodes 0..3 for allocator availability (small allocations)
[DBG 1771833098.026150] Trying node 0 ...
[DBG 1771833098.026375] node 0 -> OK
[DBG 1771833098.026808] Trying node 1 ...
[DBG 1771833098.027027] node 1 -> MemoryError: bad allocation
[DBG 1771833098.027131] Trying node 2 ...
[DBG 1771833098.027204] node 2 -> MemoryError: bad allocation
[DBG 1771833098.027268] Trying node 3 ...
[DBG 1771833098.027319] node 3 -> MemoryError: bad allocation
[DBG 1771833098.027365] Working nodes: [0]  Failures: {1: ('MemoryError', 'bad allocation'), 2: ('MemoryError', 'bad allocation'), 3: ('MemoryError', 'bad allocation')}
[DBG 1771833098.027396] Environment info:
[DBG 1771833098.051366]  platform.system(): Windows
[DBG 1771833098.051451]  platform.machine(): AMD64
[DBG 1771833098.051629]  python pointer size bits: 64
[DBG 1771833098.051822] TRY #1: InitOwnedContainer(capacity=64, node=1, alignment=64)
[DBG 1771833098.051907] ctypes allocation OK (sanity check)
[DBG 1771833098.052044] C++ threw MemoryError for this variant: bad allocation
[DBG 1771833098.052125] TRY #2: InitOwnedContainer(capacity=64, node=1, alignment=None)
[DBG 1771833098.052171] ctypes allocation OK (sanity check)
[DBG 1771833098.052241] C++ threw MemoryError for this variant: bad allocation
[DBG 1771833098.052277] TRY #3: InitOwnedContainer(capacity=64, node=0, alignment=64)
[DBG 1771833098.052304] ctypes allocation OK (sanity check)
[DBG 1771833098.052419] Init result: success True msg='Init success with capacity 64 node 0 alignment 64'
[DBG 1771833098.052469] Container init OK used params: (64, 0, 64)
[DBG 1771833098.052510] Starting full functionality test
[DBG 1771833098.052571] ReserveSlotsForProducer(4) -> 0
[DBG 1771833098.052631] GetNextProducerSequence() -> 4
[DBG 1771833098.052670] Start background reclaimer
[DBG 1771833098.102937] Stop background reclaimer
[DBG 1771833098.103389] PublishHeapPointerPairFrmAddress(addr=0x1234567890abcdef)
[DBG 1771833098.103563] Publish -> (status=PublishStatus.OK, idx=5)
[DBG 1771833098.103654] TryAssemblePairedPtrFrmProbableIdx(5) -> (1311768467294899695, 2)
[DBG 1771833098.103699] assembled_addr=0x1234567890abcdef relmode=2
[DBG 1771833098.103734] Retiring pair at probable idx 5
[DBG 1771833098.103814] After retire -> call TryReclaimingRetiredContainer (may require epoch advance)
[DBG 1771833098.103854] Try calling MenualAdvanceContainerEpoch(1) then TryReclaimingRetiredContainer again
[DBG 1771833098.103895] Poll device fences once:
[DBG 1771833098.103938] PollDeviceFencesOnce_ -> False
[DBG 1771833098.103960] Try InitContainerRegionOnIndex(4)
[DBG 1771833098.103998] InitContainerRegionOnIndex succeeded
[DBG 1771833098.104024] Attempting to publish repeatedly until FULL or until 2*capacity attempts
[DBG 1771833098.104126] Publish returned FULL after 2 attempts
[DBG 1771833098.104153] Functionality test finished OK
[DBG 1771833098.104178] Running negative tests (expected failures)
[DBG 1771833098.104286] InitOwnedContainer(0, ...) raised as expected: Capacity == 0
[DBG 1771833098.104320] Cleaning up container
(venv) PS C:\PAPER\LCIM-BitTheorium\core\TestFiles\pytests> 