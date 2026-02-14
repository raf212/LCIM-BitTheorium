// bindings/py_packed_bind.cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <optional>
#include <cstdint>
#include <limits>

#include "PackedCell.hpp"
#include "PackedStRel.h"
#include "AtomicAdaptiveBackoff.hpp"
#include "MasterClockConf.hpp"

namespace py = pybind11;
using namespace AtomicCScompact;

PYBIND11_MODULE(atomiccim_bind, m) {
    m.doc() = "pybind11 bindings for PackedCell, PackedStRel, AtomicAdaptiveBackoff, MasterClockConf";

    // -------------------------
    // PackedStRel helpers & constants (expose constants and a few helpers)
    // -------------------------
    m.attr("NO_VAL") = NO_VAL;
    m.attr("MAX_VAL") = MAX_VAL;
    m.attr("DEFAULT_INTERNAL_PRIORITY") = DEFAULT_INTERNAL_PRIORITY;

    // locality constants
    m.attr("ST_IDLE") = ST_IDLE;
    m.attr("ST_PUBLISHED") = ST_PUBLISHED;
    m.attr("ST_EXCEPTION_BIT_FAULTY") = ST_EXCEPTION_BIT_FAULTY;
    m.attr("ST_CLAIMED") = ST_CLAIMED;
    m.attr("ST_PROCESSING") = ST_PROCESSING;
    m.attr("ST_COMPLETE") = ST_COMPLETE;
    m.attr("ST_RETIRED") = ST_RETIRED;
    m.attr("ST_EPOCH_BUMP") = ST_EPOCH_BUMP;

    // relation constants (expose a handful)
    m.attr("REL_NONE") = REL_NONE;
    m.attr("REL_NODE0") = REL_NODE0;
    m.attr("REL_NODE1") = REL_NODE1;
    m.attr("REL_PAGE") = REL_PAGE;
    m.attr("REL_PATTERN") = REL_PATTERN;
    
    // expose PackedCellDataType
    // --- register enum BEFORE using it as a default arg ---
    py::enum_<PackedCellDataType>(m, "PackedCellDataType")
        .value("Unsigned", PackedCellDataType::UnsignedPCellDataType)
        .value("SignedInt", PackedCellDataType::IntPCellDataType)
        .value("Float", PackedCellDataType::FloatPCellDataType)
        .value("Char", PackedCellDataType::CharPCellDataType)
        .export_values();


    // Expose small helpers from PackedStRel.h
    m.def("make_strl4", 
        [](uint8_t priority, uint8_t locality, uint8_t rel_mask, uint8_t rel_offset, uint8_t pc_type, PackedCellDataType pcdt) -> uint16_t {
            return MakeSTRL4_t(priority, locality, rel_mask, rel_offset, pc_type, pcdt);
        },
        py::arg("priority"),
        py::arg("locality"),
        py::arg("rel_mask"),
        py::arg("rel_offset"),
        py::arg("pc_type") = 0,
        py::arg("pc_dtype") = PackedCellDataType::UnsignedPCellDataType
    );

    m.def("extract_priority_from_strl", [](uint16_t strl) { return ExtractPriorityFromSTRL(static_cast<strl16_t>(strl)); });
    m.def("extract_locality_from_strl", [](uint16_t strl) { return ExtractLocalityFromSTRL(static_cast<strl16_t>(strl)); });
    m.def("extract_pcelltype_from_strl", [](uint16_t strl) { return ExtractPCellTypeFromSTRL(static_cast<strl16_t>(strl)); });
    m.def("extract_relmask_from_strl", [](uint16_t strl) { return ExtractRelMaskFromSTRL(static_cast<strl16_t>(strl)); });
    m.def("extract_reloffset_from_strl", [](uint16_t strl) { return ExtractRelOffsetFromSTRL(static_cast<strl16_t>(strl)); });
    m.def("decode_reloffset_signed", [](uint8_t ro) { return DecodeRelOffsetSigned(ro); });
    m.def("dose_rel_match", [](uint8_t slot_relbyte, uint8_t relmask) { return DoseRelMatch(slot_relbyte, relmask); });

    // -------------------------
    // PackedCell helpers
    // -------------------------
    py::enum_<PackedMode>(m, "PackedMode")
        .value("MODE_VALUE32", PackedMode::MODE_VALUE32)
        .value("MODE_CLKVAL48", PackedMode::MODE_CLKVAL48)
        .export_values();

    m.def("mask_bits", [](unsigned n) -> uint64_t { return MaskBits(n); }, py::arg("n"));

    // Compose helpers (work with python ints)
    m.def("compose_clk48_u48", [](uint64_t clk48, uint16_t strl) {
        return static_cast<uint64_t>(PackedCell64_t::ComposeCLK48u_64(clk48, static_cast<strl16_t>(strl)));
    }, py::arg("clk48"), py::arg("strl"));

    m.def("compose_clk48_f32", [](float v, uint16_t strl) -> uint64_t {
        return static_cast<uint64_t>(PackedCell64_t::ComposeCLKVal48X_64<float>(v, static_cast<strl16_t>(strl)));
    });

    // ---------- instantiate ComposeValue32X_64 for typical types ----------
    m.def("compose_value32_u32", [](uint32_t v, uint16_t clk16, uint16_t strl) {
        return static_cast<uint64_t>(PackedCell64_t::ComposeValue32u_64(static_cast<val32_t>(v), static_cast<clk16_t>(clk16), static_cast<strl16_t>(strl)));
    }, py::arg("value32"), py::arg("clk16"), py::arg("strl"));


    m.def("compose_value32_i32", [](int32_t v, uint16_t clk16, uint16_t strl) -> uint64_t {
        return static_cast<uint64_t>(PackedCell64_t::ComposeValue32X_64<int32_t>(v, static_cast<clk16_t>(clk16), static_cast<strl16_t>(strl)));
    }, py::arg("value32"), py::arg("clk16"), py::arg("strl"));

    m.def("compose_value32_f32", [](float v, uint16_t clk16, uint16_t strl) -> uint64_t {
        return static_cast<uint64_t>(PackedCell64_t::ComposeValue32X_64<float>(v, static_cast<clk16_t>(clk16), static_cast<strl16_t>(strl)));
    }, py::arg("value32"), py::arg("clk16"), py::arg("strl"));

    // ---------- instantiate ExtractAnyPackedValueX for common types ----------
    m.def("extract_any_u32", [](uint64_t packed) -> uint32_t {
        return PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(static_cast<packed64_t>(packed));
    });

    m.def("extract_any_i32", [](uint64_t packed) -> int32_t {
        return PackedCell64_t::ExtractAnyPackedValueX<int32_t>(static_cast<packed64_t>(packed));
    });

    m.def("extract_any_f32", [](uint64_t packed) -> float {
        return PackedCell64_t::ExtractAnyPackedValueX<float>(static_cast<packed64_t>(packed));
    });

    // smaller ints
    m.def("extract_any_u16", [](uint64_t packed) -> uint16_t {
        return PackedCell64_t::ExtractAnyPackedValueX<uint16_t>(static_cast<packed64_t>(packed));
    });
    m.def("extract_any_i16", [](uint64_t packed) -> int16_t {
        return PackedCell64_t::ExtractAnyPackedValueX<int16_t>(static_cast<packed64_t>(packed));
    });

    // Extractors
    m.def("extract_strl", [](uint64_t p) -> uint16_t { return PackedCell64_t::ExtractSTRL(static_cast<packed64_t>(p)); });
    m.def("extract_full_rel_from_packed", [](uint64_t p) -> uint8_t { return PackedCell64_t::ExtractFullRelFromPacked(static_cast<packed64_t>(p)); });
    m.def("extract_value32", [](uint64_t p) -> uint32_t { return PackedCell64_t::ExtractValue32(static_cast<packed64_t>(p)); });
    m.def("extract_clk16", [](uint64_t p) -> uint16_t { return PackedCell64_t::ExtractClk16(static_cast<packed64_t>(p)); });
    m.def("extract_clk48", [](uint64_t p) -> uint64_t { return PackedCell64_t::ExtractClk48(static_cast<packed64_t>(p)); });
    m.def("extract_priority_from_packed", [](uint64_t p) -> uint8_t { return PackedCell64_t::ExtractPriorityFromPacked(static_cast<packed64_t>(p)); });
    m.def("extract_locality_from_packed", [](uint64_t p) -> uint8_t { return PackedCell64_t::ExtractLocalityFromPacked(static_cast<packed64_t>(p)); });
    m.def("extract_pcelltype_from_packed", [](uint64_t p) -> uint8_t { return PackedCell64_t::ExtractPCellTypeFromPacked(static_cast<packed64_t>(p)); });
    m.def("is_packed_cell_val32", [](uint64_t p) -> bool { return PackedCell64_t::IsPackedCellVal32(static_cast<packed64_t>(p)); });
    m.def("extract_relmask_from_packed", [](uint64_t p) -> uint8_t { return PackedCell64_t::ExtractRelMaskFromPacked(static_cast<packed64_t>(p)); });
    m.def("extract_reloffset_from_packed", [](uint64_t p) -> uint8_t { return PackedCell64_t::ExtractRelOffsetFromPacked(static_cast<packed64_t>(p)); });
    // setters that return new packed value
    m.def("set_priority_in_packed", [](uint64_t p, uint8_t priority) -> uint64_t {
        return PackedCell64_t::SetPriorityInPacked(static_cast<packed64_t>(p), priority);
    });
    m.def("set_locality_in_packed", [](uint64_t p, uint8_t local_state) -> uint64_t {
        return PackedCell64_t::SetLocalityInPacked(static_cast<packed64_t>(p), local_state);
    });
    m.def("blind_mode_switch_of_packed", [](int out_mode, uint64_t p) -> uint64_t {
        return PackedCell64_t::BlindModeSwitchOfPacked(static_cast<PackedMode>(out_mode), static_cast<packed64_t>(p));
    });
    m.def("set_relmask_in_packed", [](uint64_t p, uint8_t relmask) -> uint64_t {
        return PackedCell64_t::SetRelMaskInPacked(static_cast<packed64_t>(p), relmask);
    });
    m.def("set_reloffset_in_packed", [](uint64_t p, uint8_t reloffset) -> uint64_t {
        return PackedCell64_t::SetRelOffsetInPacked(static_cast<packed64_t>(p), reloffset);
    });

    // AsValue: expose as bytes view (return python bytes representing 8 bytes)
    m.def("packed_as_bytes", [](uint64_t p) {
        char buf[8];
        std::memcpy(buf, &p, sizeof(p));
        return py::bytes(buf, sizeof(buf));
    });


    // -------------------------
    // AtomicAdaptiveBackoff & helpers
    // -------------------------
    py::enum_<AtomicAdaptiveBackoff::PCBAction>(m, "PCBAction")
        .value("SPIN_IMMEDIATE", AtomicAdaptiveBackoff::PCBAction::SPIN_IMMEDIATE)
        .value("SPIN_FOR_US", AtomicAdaptiveBackoff::PCBAction::SPIN_FOR_US)
        .value("PARK_FOR_US", AtomicAdaptiveBackoff::PCBAction::PARK_FOR_US)
        .value("BLOCK_WAIT", AtomicAdaptiveBackoff::PCBAction::BLOCK_WAIT)
        .export_values();

    py::class_<AtomicAdaptiveBackoff::PCBDecision>(m, "PCBDecision")
        .def(py::init<>())
        .def_readwrite("action", &AtomicAdaptiveBackoff::PCBDecision::Action)
        .def_readwrite("suggested_us", &AtomicAdaptiveBackoff::PCBDecision::SuggestedUs)
        .def_readwrite("est_hazard_per_sec", &AtomicAdaptiveBackoff::PCBDecision::EstHazPerSec);

    // Timer48
    py::class_<Timer48>(m, "Timer48")
        .def(py::init<>())
        .def_readwrite("TicksPerSec", &Timer48::TicksPerSec_)
        .def("now_ticks", &Timer48::NowTicks);

    // AtomicAdaptiveBackoff.PCBCfg (expose a minimal config)
    py::class_<AtomicAdaptiveBackoff::PCBCfg>(m, "PCBCfg")
        .def(py::init<>())
        .def_readwrite("DownShift", &AtomicAdaptiveBackoff::PCBCfg::DownShift)
        .def_readwrite("BaseUS", &AtomicAdaptiveBackoff::PCBCfg::BaseUS)
        .def_readwrite("SpinThresholUS", &AtomicAdaptiveBackoff::PCBCfg::SpinThresholUS)
        .def_readwrite("MaxParkUS", &AtomicAdaptiveBackoff::PCBCfg::MaxParkUS)
        .def_readwrite("CostSpinPerSec", &AtomicAdaptiveBackoff::PCBCfg::CostSpinPerSec)
        .def_readwrite("CostPark", &AtomicAdaptiveBackoff::PCBCfg::CostPark)
        .def_readwrite("PriorityGama", &AtomicAdaptiveBackoff::PCBCfg::PriorityGama)
        .def_readwrite("Jitter", &AtomicAdaptiveBackoff::PCBCfg::Jitter);

    py::class_<AtomicAdaptiveBackoff>(m, "AtomicAdaptiveBackoff")
        .def(py::init<>())
        .def(py::init<const AtomicAdaptiveBackoff::PCBCfg&, PackedMode, Timer48>(),
             py::arg("cfg") = AtomicAdaptiveBackoff::PCBCfg(),
             py::arg("mode") = PackedMode::MODE_VALUE32,
             py::arg("timer") = Timer48())
        .def("decide_for_slot", [](AtomicAdaptiveBackoff &self, uint64_t slot_payload, std::optional<uint64_t> now_ticks_opt) {
            auto dec = self.DecideForSlot(static_cast<packed64_t>(slot_payload), now_ticks_opt);
            // return tuple (action:int, suggested_us:uint64, est_hazard:double)
            return py::make_tuple(static_cast<int>(dec.Action), dec.SuggestedUs, dec.EstHazPerSec);
        }, py::arg("slot_payload"), py::arg("now_ticks") = std::optional<uint64_t>{})
        .def("observe_completion", [](AtomicAdaptiveBackoff &self, uint64_t pub_p, std::optional<uint64_t> observe_time_ticks) {
            self.ObserveCompletation(static_cast<packed64_t>(pub_p), observe_time_ticks);
        }, py::arg("published_payload"), py::arg("observe_time_ticks") = std::optional<uint64_t>{})
        .def("set_cost", &AtomicAdaptiveBackoff::SetCost);

    // Expose EMAEstimatorAPC methods as convenience (mean, hazard) via wrapper lambdas
    py::class_<EMAEstimatorAPC>(m, "EMAEstimatorAPC")
        .def(py::init<>())
        .def("observe_ticks", &EMAEstimatorAPC::ObserveTicks)
        .def("mean_ticks", [](const EMAEstimatorAPC &e) -> py::object {
            auto m = e.MeanTicks();
            if (m.has_value()) return py::float_(m.value());
            return py::none();
        })
        .def("hazard_per_sec", [](const EMAEstimatorAPC &e, const Timer48 &t) -> py::object {
            auto h = e.HazardPerSec(t);
            if (h.has_value()) return py::float_(h.value());
            return py::none();
        });

    py::class_<HazardEstimatorPC>(m, "HazardEstimatorPC")
        .def(py::init<>())
        .def("observe_us", &HazardEstimatorPC::ObserveUS)
        .def("prob_hazard_at_us", [](const HazardEstimatorPC &h, uint64_t age_us) -> py::object {
            auto p = h.ProbHazardAtUS(age_us);
            if (p.has_value()) return py::float_(p.value());
            return py::none();
        });

    // -------------------------
    // MasterClockConf
    // -------------------------
    py::class_<MasterClockConf>(m, "MasterClockConf")
        .def(py::init<Timer48&, int>(), py::arg("timer48"), py::arg("used_node") = 0)
        .def("init_master_clock_slots", &MasterClockConf::InitMasterClockSlots, py::arg("max_slots"), py::arg("alignment") = 64)
        .def("free_master_clock_slots", &MasterClockConf::FreeMasterClockSlots)
        .def("register_master_clock_slot", [](MasterClockConf &self, uint64_t given_init_clk, std::optional<size_t> m_id_opt) -> py::size_t {
            size_t id = (m_id_opt.has_value()) ? self.RegisterMasterClockSlot(static_cast<packed64_t>(given_init_clk), m_id_opt.value()) : self.RegisterMasterClockSlot(static_cast<packed64_t>(given_init_clk));
            return id;
        }, py::arg("given_init_clk") = 0, py::arg("m_id") = std::optional<size_t>{})
        .def("attach_thread_mclock_id", &MasterClockConf::AttachThreadMClockID)
        .def("read_master_clock_packed", &MasterClockConf::ReadMasterClockPacked)
        .def_readwrite("UsedNode", &MasterClockConf::UsedNode);

    // done
}
