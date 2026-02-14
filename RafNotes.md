1. Have to Rewrite -> While working on Bindings->FUTURE::
    a. core\bindings\PackedCellCoreBindings.cpp
    b. core\src\AtomicCimCore.cpp
    c. core\CMakeLists.txt
2. Convert last 2 bit of RelOffset to hold unsigned,Int,float,string
    Write BlindPCellDataTypeSwitch() -> will blindly switch datatype of an existing PackedCell
2. Encode a Exception return in strl16 might be ST_EXCEPTION_BIT_FAULTY then a explicit function :
    <b><i>IsThisPackedCellFaulty(pack64_t p)</i></b>
2. At This point Adding (unsigned <-> int <-> float) convertion should be explicitly accomodate <b>Explicit ComposeValue32u_64 & ComposeValue32u_64 with int32 and float32 input parameters</b>
3. I need 2 functionf one each for each type of packed cell AtomicIncrementOrDecrementOfPCPreInitiated() It will use typename for both signed and unsigned