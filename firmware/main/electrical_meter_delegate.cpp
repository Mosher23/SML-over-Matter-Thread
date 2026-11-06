#include "electrical_meter_delegate.h"

#include <app/reporting/reporting.h>
#include <app/data-model/List.h>
#include <clusters/ElectricalPowerMeasurement/Attributes.h>
#include <clusters/ElectricalPowerMeasurement/Enums.h>
#include <clusters/ElectricalPowerMeasurement/Structs.h>
#include <lib/support/CodeUtils.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::ElectricalPowerMeasurement;

namespace {
const Structs::MeasurementAccuracyRangeStruct::Type kActivePowerAccuracyRanges[] = {
    {
        .rangeMin = -100'000'000, // -100 kW, expressed in mW
        .rangeMax = 100'000'000,  //  100 kW, expressed in mW
        .percentMax = MakeOptional(static_cast<Percent100ths>(100)),
        .percentMin = NullOptional,
        .percentTypical = NullOptional,
        .fixedMax = NullOptional,
        .fixedMin = NullOptional,
        .fixedTypical = NullOptional,
    },
};

const Structs::MeasurementAccuracyStruct::Type kAccuracies[] = {
    {
        .measurementType = MeasurementTypeEnum::kActivePower,
        .measured = true,
        .minMeasuredValue = -100'000'000,
        .maxMeasuredValue = 100'000'000,
        .accuracyRanges = DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kActivePowerAccuracyRanges),
    },
};
} // namespace

PowerModeEnum SmartMeterPowerDelegate::GetPowerMode()
{
    return PowerModeEnum::kAc;
}

uint8_t SmartMeterPowerDelegate::GetNumberOfMeasurementTypes()
{
    return static_cast<uint8_t>(MATTER_ARRAY_SIZE(kAccuracies));
}

CHIP_ERROR SmartMeterPowerDelegate::StartAccuracyRead() { return CHIP_NO_ERROR; }

CHIP_ERROR SmartMeterPowerDelegate::GetAccuracyByIndex(uint8_t index, MeasurementAccuracy &accuracy)
{
    if (index >= MATTER_ARRAY_SIZE(kAccuracies)) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    accuracy = kAccuracies[index];
    return CHIP_NO_ERROR;
}

CHIP_ERROR SmartMeterPowerDelegate::EndAccuracyRead() { return CHIP_NO_ERROR; }
CHIP_ERROR SmartMeterPowerDelegate::StartRangesRead() { return CHIP_NO_ERROR; }
CHIP_ERROR SmartMeterPowerDelegate::GetRangeByIndex(uint8_t, MeasurementRange &) { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
CHIP_ERROR SmartMeterPowerDelegate::EndRangesRead() { return CHIP_NO_ERROR; }
CHIP_ERROR SmartMeterPowerDelegate::StartHarmonicCurrentsRead() { return CHIP_NO_ERROR; }
CHIP_ERROR SmartMeterPowerDelegate::GetHarmonicCurrentsByIndex(uint8_t, HarmonicMeasurement &) { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
CHIP_ERROR SmartMeterPowerDelegate::EndHarmonicCurrentsRead() { return CHIP_NO_ERROR; }
CHIP_ERROR SmartMeterPowerDelegate::StartHarmonicPhasesRead() { return CHIP_NO_ERROR; }
CHIP_ERROR SmartMeterPowerDelegate::GetHarmonicPhasesByIndex(uint8_t, HarmonicMeasurement &) { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
CHIP_ERROR SmartMeterPowerDelegate::EndHarmonicPhasesRead() { return CHIP_NO_ERROR; }

SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetVoltage() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetActiveCurrent() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetReactiveCurrent() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetApparentCurrent() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetActivePower() { return mActivePower; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetReactivePower() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetApparentPower() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetRMSVoltage() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetRMSCurrent() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetRMSPower() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetFrequency() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetPowerFactor() { return DataModel::NullNullable; }
SmartMeterPowerDelegate::NullableInt64 SmartMeterPowerDelegate::GetNeutralCurrent() { return DataModel::NullNullable; }

CHIP_ERROR SmartMeterPowerDelegate::SetActivePowerMilliwatts(int64_t value)
{
    NullableInt64 next(value);
    if (mActivePower != next) {
        mActivePower = next;
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id,
                                               Attributes::ActivePower::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR SmartMeterPowerDelegate::ClearActivePower()
{
    if (!mActivePower.IsNull()) {
        mActivePower.SetNull();
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id,
                                               Attributes::ActivePower::Id);
    }
    return CHIP_NO_ERROR;
}
