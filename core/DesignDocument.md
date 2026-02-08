1. To handle exception where BlindModeSwitchOfPacked can have Exception MODE new LOCALITY->ST_EXCEPTION_BIT_FAULTY::Introduced.
2. Convert last 2 bit of RelOffset to hold unsigned,Int,float,string
    fix all the set functions in PackedCell.hpp
2. At This point Adding (unsigned <-> int <-> float) convertion should be explicitly accomodate <b>Explicit ComposeValue32u_64 & ComposeValue32u_64 with int32 and float32 input parameters</b>
3. I need 2 functionf one each for each type of packed cell AtomicIncrementOrDecrementOfPCPreInitiated() It will use typename for both signed and unsigned