#pragma once

#include <app/clusters/electrical-power-measurement-server/ElectricalPowerMeasurementDelegate.h>
#include <app/data-model/Nullable.h>

class SmartMeterPowerDelegate final
    : public chip::app::Clusters::ElectricalPowerMeasurement::Delegate {
public:
    using NullableInt64 = chip::app::DataModel::Nullable<int64_t>;
    using PowerModeEnum = chip::app::Clusters::ElectricalPowerMeasurement::PowerModeEnum;
    using MeasurementAccuracy = chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyStruct::Type;
    using MeasurementRange = chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementRangeStruct::Type;
    using HarmonicMeasurement = chip::app::Clusters::ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type;

    PowerModeEnum GetPowerMode() override;
    uint8_t GetNumberOfMeasurementTypes() override;

    CHIP_ERROR StartAccuracyRead() override;
    CHIP_ERROR GetAccuracyByIndex(uint8_t index, MeasurementAccuracy &accuracy) override;
    CHIP_ERROR EndAccuracyRead() override;

    CHIP_ERROR StartRangesRead() override;
    CHIP_ERROR GetRangeByIndex(uint8_t index, MeasurementRange &range) override;
    CHIP_ERROR EndRangesRead() override;

    CHIP_ERROR StartHarmonicCurrentsRead() override;
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t index, HarmonicMeasurement &measurement) override;
    CHIP_ERROR EndHarmonicCurrentsRead() override;

    CHIP_ERROR StartHarmonicPhasesRead() override;
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t index, HarmonicMeasurement &measurement) override;
    CHIP_ERROR EndHarmonicPhasesRead() override;

    NullableInt64 GetVoltage() override;
    NullableInt64 GetActiveCurrent() override;
    NullableInt64 GetReactiveCurrent() override;
    NullableInt64 GetApparentCurrent() override;
    NullableInt64 GetActivePower() override;
    NullableInt64 GetReactivePower() override;
    NullableInt64 GetApparentPower() override;
    NullableInt64 GetRMSVoltage() override;
    NullableInt64 GetRMSCurrent() override;
    NullableInt64 GetRMSPower() override;
    NullableInt64 GetFrequency() override;
    NullableInt64 GetPowerFactor() override;
    NullableInt64 GetNeutralCurrent() override;

    CHIP_ERROR SetActivePowerMilliwatts(int64_t value);
    CHIP_ERROR ClearActivePower();

private:
    // No power measurement is available until the first valid SML reading.
    // A synthetic zero would corrupt power history and derived statistics.
    NullableInt64 mActivePower{chip::app::DataModel::NullNullable};
};
