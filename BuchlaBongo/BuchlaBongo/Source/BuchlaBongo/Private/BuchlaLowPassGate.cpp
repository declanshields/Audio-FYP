#include "Internationalization/Text.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundEnumRegistrationMacro.h"
#include "MetasoundParamHelper.h"
#include "MetasoundPrimitives.h"
#include "MetasoundStandardNodesNames.h"
#include "MetasoundDataTypeRegistrationMacro.h"
#include "MetasoundTrigger.h"
#include "MetasoundTime.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundStandardNodesCategories.h"
#include "MetasoundFacade.h"
#include "DSP/Dsp.h"
#include "DSP/Filter.h"
#include "DSP/InterpolatedOnePole.h"

#define LOCTEXT_NAMESPACE "BuchlaBongo_LPG"

namespace Metasound
{
	enum class ELowPassGateMode : int32
	{
		LowPass,
		VCA,
		Both
	};

	DECLARE_METASOUND_ENUM(ELowPassGateMode, ELowPassGateMode::LowPass, BUCHLABONGO_API, FEnumELowPassGateMode, FEnumLowPassGateModeInfo, FEnumLowPassGateModeReadRef, FEnumLowPassGateModeWriteRef);
	DEFINE_METASOUND_ENUM_BEGIN(ELowPassGateMode, FEnumELowPassGateMode, "LowPassGateMode")
		DEFINE_METASOUND_ENUM_ENTRY(ELowPassGateMode::LowPass, "LowPassDescription", "Low Pass", "LowPassTT", "Low Pass Mode"),
		DEFINE_METASOUND_ENUM_ENTRY(ELowPassGateMode::VCA, "VCADescription", "VCA", "VCATT", "VCA Mode"),
		DEFINE_METASOUND_ENUM_ENTRY(ELowPassGateMode::Both, "BothDescription", "Both", "BothTT", "Both Mode")
		DEFINE_METASOUND_ENUM_END()

	namespace LowPassGate
	{
		METASOUND_PARAM(InputTrigger, "Trigger", "Trigger to start envelope generator");
		METASOUND_PARAM(InputAttackTime, "Attack Time", "The attack time of the envelope");
		METASOUND_PARAM(InputDecayTime, "Decay Time", "The decay time of the envelope");
		METASOUND_PARAM(InputAttackCurve, "Attack Curve", "1.0 = linear growth, <1.0 = logorithmic growth, >1.0 = exponential growth");
		METASOUND_PARAM(InputDecayCurve, "Decay Curve", "1.0 = linear decay, <1.0 = exponential decay, >1.0 = logorithmic decay");
		METASOUND_PARAM(InputAudio, "In", "Audio input");
		METASOUND_PARAM(InputCutOff, "Cut Off", "Cut off frequency");
		METASOUND_PARAM(InputMode, "Mode", "Low Pass Gate Mode");

		METASOUND_PARAM(OutputTrigger, "On Trigger", "Triggers when envelope is triggered");
		METASOUND_PARAM(OutputOnDone, "On Done", "Triggers when envelope finishes");
		METASOUND_PARAM(OutputEnvelope, "Envelope", "Output Envelope");
		METASOUND_PARAM(OutputAudio, "Out", "Output Audio");
	}

	struct FEnvelopeState
	{
		int32 CurrentSampleIndex = INDEX_NONE;
		int32 AttackSampleCount = 1;
		int32 DecaySampleCount = 1;
		float AttackCurveFactor = 0.0f;
		float DecayCurveFactor = 0.0f;
		Audio::FExponentialEase EnvelopeEase;
		float StartingEnvelopeValue = 0.0f;
		float CurrentEnvelopeValue = 0.0f;

		bool bLooping = false;
		bool bHardReset = false;

		void Reset()
		{
			CurrentSampleIndex = INDEX_NONE;
			AttackSampleCount = 1;
			DecaySampleCount = 1;

			AttackCurveFactor = 0.0f;
			DecayCurveFactor = 0.0f;

			StartingEnvelopeValue = 0.0f;
			CurrentEnvelopeValue = 0.0f;

			bLooping = false;
			bHardReset = false;
			EnvelopeEase.Init(0.f, 0.01f);
		}
	};

	struct Envelope
	{
		static void GetNextEnvelopeOutput(FEnvelopeState& InState, int32 StartFrame, int32 EndFrame, TArray<int32>& OutFinishedFrames, float& OutEnvelopeValue)
		{
			if (StartFrame > 0 || InState.CurrentSampleIndex == INDEX_NONE)
			{
				OutEnvelopeValue = 0.0f;
				return;
			}

			if (InState.CurrentSampleIndex < InState.AttackSampleCount)
			{
				if (InState.AttackSampleCount > 1)
				{
					float AttackFraction = (float)InState.CurrentSampleIndex++ / InState.AttackSampleCount;
					OutEnvelopeValue = InState.StartingEnvelopeValue + (1.0f - InState.StartingEnvelopeValue) * FMath::Pow(AttackFraction, InState.AttackCurveFactor);
				}
				else
				{
					InState.CurrentSampleIndex++;
					OutEnvelopeValue = 1.f;
				}
			}
			else
			{
				int32 TotalEnvSampleCount = (InState.AttackSampleCount + InState.DecaySampleCount);

				if (InState.CurrentSampleIndex < TotalEnvSampleCount)
				{
					int32 SampleCountInDecayState = InState.CurrentSampleIndex++ - InState.AttackSampleCount;
					float DecayFraction = (float)SampleCountInDecayState / InState.DecaySampleCount;
					OutEnvelopeValue = 1.0f - FMath::Pow(DecayFraction, InState.DecayCurveFactor);
				}
				else
				{
					InState.CurrentSampleIndex = INDEX_NONE;
					OutEnvelopeValue = 0.0f;
					OutFinishedFrames.Add(0);
				}
			}
		}

		static void GetInitialOutputEnvelope(float& OutputEnvelope)
		{
			OutputEnvelope = 0.f;
		}
	};

	class FLowPassGateOperator : public TExecutableOperator<FLowPassGateOperator>
	{
	public:
		static const FNodeClassMetadata& GetNodeInfo();
		static const FVertexInterface& GetVertexInterface();
		static TUniquePtr<IOperator> CreateOperator(const FCreateOperatorParams& InParams, FBuildErrorArray& OutErrors);

		FLowPassGateOperator(const FOperatorSettings& InSettings,
			const FTriggerReadRef& InTriggerIn,
			const FTimeReadRef& InAttackTime,
			const FTimeReadRef& InDecayTime,
			const FFloatReadRef& InAttackCurveFactor,
			const FFloatReadRef& InDecayCurveFactor,
			const FAudioBufferReadRef& InAudioInput,
			const FFloatReadRef& InCutOff,
			const FEnumLowPassGateModeReadRef& InGateMode);

		virtual FDataReferenceCollection GetInputs() const override;
		virtual FDataReferenceCollection GetOutputs() const override;

		void UpdateParams();
		void Execute();

		void HandleLowPassFilter();
		void CalculateEnvelope();

	private:
		FTriggerReadRef TriggerAttackIn;
		FTimeReadRef AttackTime;
		FTimeReadRef DecayTime;
		FFloatReadRef AttackCurveFactor;
		FFloatReadRef DecayCurveFactor;
		FAudioBufferReadRef AudioInput;
		FFloatReadRef CutOffFrequency;
		FEnumLowPassGateModeReadRef Mode;

		FTriggerWriteRef OnAttackTrigger;
		FTriggerWriteRef OnDone;
		TDataWriteReference<float> OutEnvelope;
		FAudioBufferWriteRef AudioOutput;

		float SampleRate = 0.0f;
		int32 NumFramesPerBlock = 0;

		FEnvelopeState EnvelopeState;
		Audio::FStateVariableFilter VariableFilter;
		float PreviousFrequency{ -1.f };
		float PreviousResonance{ -1.f };
		float PreviousBandStopControl{ -1.f };
	};

	const FNodeClassMetadata& FLowPassGateOperator::GetNodeInfo()
	{
		auto CreateNodeClassMetadata = []()->FNodeClassMetadata
		{
			FNodeClassMetadata Info;
			Info.ClassName = { FName("BuchlaBongo"), TEXT("Buchla Low Pass Gate"), FName("Audio") };
			Info.MajorVersion = 1;
			Info.MinorVersion = 0;
			Info.DisplayName = METASOUND_LOCTEXT("LPGDisplayName", "Buchla Low Pass Gate");
			Info.Description = METASOUND_LOCTEXT("LPGDescription", "Low Pass Gate");
			Info.Author = TEXT("Declan Shields");
			Info.DefaultInterface = GetVertexInterface();
			Info.CategoryHierarchy.Emplace(NodeCategories::Filters);

			return Info;
		};

		static const FNodeClassMetadata Info = CreateNodeClassMetadata();

		return Info;
	}

	const FVertexInterface& FLowPassGateOperator::GetVertexInterface()
	{
		using namespace LowPassGate;

		static const FVertexInterface Interface(
			FInputVertexInterface(
				TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTrigger)),
				TInputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputAttackTime), 0.01f),
				TInputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputDecayTime), 1.0f),
				TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputAttackCurve), 1.0f),
				TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputDecayCurve), 1.0f),
				TInputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputAudio)),
				TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputCutOff), 1000.0f),
				TInputDataVertex<FEnumELowPassGateMode>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputMode))
			),
			FOutputVertexInterface(
				TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTrigger)),
				TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputOnDone)),
				TOutputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputEnvelope)),
				TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputAudio))
			)
		);

		return Interface;
	}

	TUniquePtr<IOperator> FLowPassGateOperator::CreateOperator(const FCreateOperatorParams& InParams, FBuildErrorArray& OutErrors)
	{
		using namespace LowPassGate;

		const FInputVertexInterface& InputInterface = GetVertexInterface().GetInputInterface();

		FTriggerReadRef TriggerIn = InParams.InputDataReferences.GetDataReadReferenceOrConstruct<FTrigger>(METASOUND_GET_PARAM_NAME(InputTrigger), InParams.OperatorSettings);
		FTimeReadRef AttackTime = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<FTime>(InputInterface, METASOUND_GET_PARAM_NAME(InputAttackTime), InParams.OperatorSettings);
		FTimeReadRef DecayTime = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<FTime>(InputInterface, METASOUND_GET_PARAM_NAME(InputDecayTime), InParams.OperatorSettings);
		FFloatReadRef AttackCurveFactor = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<float>(InputInterface, METASOUND_GET_PARAM_NAME(InputAttackCurve), InParams.OperatorSettings);
		FFloatReadRef DecayCurveFactor = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<float>(InputInterface, METASOUND_GET_PARAM_NAME(InputDecayCurve), InParams.OperatorSettings);
		FAudioBufferReadRef AudioIn = InParams.InputDataReferences.GetDataReadReferenceOrConstruct<FAudioBuffer>(METASOUND_GET_PARAM_NAME(InputAudio), InParams.OperatorSettings);
		FFloatReadRef CutOff = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<float>(InputInterface, METASOUND_GET_PARAM_NAME(InputCutOff), InParams.OperatorSettings);
		FEnumLowPassGateModeReadRef InMode = InParams.InputDataReferences.GetDataReadReferenceOrConstruct<FEnumELowPassGateMode>(METASOUND_GET_PARAM_NAME(InputMode));

		return MakeUnique<FLowPassGateOperator>(InParams.OperatorSettings, TriggerIn, AttackTime, DecayTime, AttackCurveFactor, DecayCurveFactor, AudioIn, CutOff, InMode);
	}

	FLowPassGateOperator::FLowPassGateOperator(const FOperatorSettings& InSettings,
		const FTriggerReadRef& InTriggerIn,
		const FTimeReadRef& InAttackTime,
		const FTimeReadRef& InDecayTime,
		const FFloatReadRef& InAttackCurveFactor,
		const FFloatReadRef& InDecayCurveFactor,
		const FAudioBufferReadRef& InAudioInput,
		const FFloatReadRef& InCutOff,
		const FEnumLowPassGateModeReadRef& InGateMode) : TriggerAttackIn(InTriggerIn)
		, AttackTime(InAttackTime)
		, DecayTime(InDecayTime)
		, AttackCurveFactor(InAttackCurveFactor)
		, DecayCurveFactor(InDecayCurveFactor)
		, AudioInput(InAudioInput)
		, CutOffFrequency(InCutOff)
		, Mode(InGateMode)
		, OnAttackTrigger(TDataWriteReferenceFactory<FTrigger>::CreateAny(InSettings))
		, OnDone(TDataWriteReferenceFactory<FTrigger>::CreateAny(InSettings))
		, OutEnvelope(TDataWriteReferenceFactory<float>::CreateAny(InSettings))
		, AudioOutput(FAudioBufferWriteRef::CreateNew(InSettings))
	{
		NumFramesPerBlock = InSettings.GetNumFramesPerBlock();
		EnvelopeState.EnvelopeEase.SetEaseFactor(0.01f);
		SampleRate = InSettings.GetSampleRate();
		VariableFilter.Init(SampleRate, 1);
	}

	FDataReferenceCollection FLowPassGateOperator::GetInputs() const
	{
		using namespace LowPassGate;

		FDataReferenceCollection Inputs;
		Inputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputTrigger), TriggerAttackIn);
		Inputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputAttackTime), AttackTime);
		Inputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputDecayTime), DecayTime);
		Inputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputAttackCurve), AttackCurveFactor);
		Inputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputDecayCurve), DecayCurveFactor);
		Inputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputAudio), AudioInput);
		Inputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputCutOff), CutOffFrequency);
		Inputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputMode), Mode);

		return Inputs;
	}

	FDataReferenceCollection FLowPassGateOperator::GetOutputs() const
	{
		using namespace LowPassGate;

		FDataReferenceCollection Outputs;
		Outputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTrigger), OnAttackTrigger);
		Outputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputOnDone), OnDone);
		Outputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputEnvelope), OutEnvelope);
		Outputs.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputAudio), AudioOutput);

		return Outputs;
	}

	void FLowPassGateOperator::UpdateParams()
	{
		float AttackTimeSeconds = AttackTime->GetSeconds();
		float DecayTimeSeconds = DecayTime->GetSeconds();
		EnvelopeState.AttackSampleCount = FMath::Max(1, SampleRate * AttackTimeSeconds);
		EnvelopeState.DecaySampleCount = FMath::Max(1, SampleRate * DecayTimeSeconds);
		EnvelopeState.AttackCurveFactor = FMath::Max(KINDA_SMALL_NUMBER, *AttackCurveFactor);
		EnvelopeState.DecayCurveFactor = FMath::Max(KINDA_SMALL_NUMBER, *DecayCurveFactor);
	}

	void FLowPassGateOperator::CalculateEnvelope()
	{
		OnAttackTrigger->AdvanceBlock();
		OnDone->AdvanceBlock();

		UpdateParams();

		TriggerAttackIn->ExecuteBlock(
			[&](int32 StartFrame, int32 EndFrame)
			{
				TArray<int32> FinishedFrames;
				Envelope::GetNextEnvelopeOutput(EnvelopeState, StartFrame, EndFrame, FinishedFrames, *OutEnvelope);

				for (int32 FrameFinished : FinishedFrames)
				{
					OnDone->TriggerFrame(FrameFinished);
				}
			},
			[&](int32 StartFrame, int32 EndFrame)
			{
				UpdateParams();
				EnvelopeState.CurrentSampleIndex = 0;
				EnvelopeState.StartingEnvelopeValue = EnvelopeState.bHardReset ? 0 : EnvelopeState.CurrentEnvelopeValue;
				EnvelopeState.EnvelopeEase.SetValue(EnvelopeState.StartingEnvelopeValue, true);

				TArray<int32> FinishedFrames;
				Envelope::GetNextEnvelopeOutput(EnvelopeState, StartFrame, EndFrame, FinishedFrames, *OutEnvelope);
				for (int32 FrameFinished : FinishedFrames)
				{
					OnDone->TriggerFrame(FrameFinished);
				}
				OnAttackTrigger->TriggerFrame(StartFrame);
			}
		);
	}

	void FLowPassGateOperator::HandleLowPassFilter()
	{
		const float CurrentFrequency = FMath::Clamp(*CutOffFrequency, 0.f, (0.5f * SampleRate));
		const float CurrentResonance = FMath::Clamp(0.f, 0.f, 10.f);
		const float CurrentBandStopControl = FMath::Clamp(0.f, 0.f, 1.f);

		bool bNeedsUpdate =
			(!FMath::IsNearlyEqual(PreviousFrequency, CurrentFrequency))
			|| (!FMath::IsNearlyEqual(PreviousResonance, CurrentResonance))
			|| (!FMath::IsNearlyEqual(PreviousBandStopControl, CurrentBandStopControl));

		if (bNeedsUpdate)
		{
			VariableFilter.SetQ(CurrentResonance);
			VariableFilter.SetFrequency(CurrentFrequency);
			VariableFilter.SetBandStopControl(CurrentBandStopControl);

			VariableFilter.Update();

			PreviousFrequency = CurrentFrequency;
			PreviousResonance = CurrentResonance;
			PreviousBandStopControl = CurrentBandStopControl;
		}
	}

	void FLowPassGateOperator::Execute()
	{
		if (*Mode == ELowPassGateMode::LowPass)
		{
			HandleLowPassFilter();
			VariableFilter.ProcessAudio(AudioInput->GetData(), AudioInput->Num(), AudioOutput->GetData());
		}
		else if (*Mode == ELowPassGateMode::VCA)
		{
			CalculateEnvelope();

			const float* InputAudio = AudioInput->GetData();
			float* OutputAudio = AudioOutput->GetData();
			for (int i = 0; i < AudioInput->Num(); ++i)
			{
				OutputAudio[i] = InputAudio[i] * (*OutEnvelope);
			}
		}
		else if (*Mode == ELowPassGateMode::Both)
		{
			TRange<float> InRange = TRange<float>(0.0f, 20000.0f);
			TRange<float> OutRange = TRange<float>(0.0f, 1.0f);
			float ClampedFreq = FMath::GetMappedRangeValueClamped(InRange, OutRange, *CutOffFrequency);

			OnAttackTrigger->AdvanceBlock();
			OnDone->AdvanceBlock();

			UpdateParams();

			TriggerAttackIn->ExecuteBlock(
				[&](int32 StartFrame, int32 EndFrame)
				{
					TArray<int32> FinishedFrames;
					Envelope::GetNextEnvelopeOutput(EnvelopeState, StartFrame, EndFrame, FinishedFrames, *OutEnvelope);

					for (int32 FrameFinished : FinishedFrames)
					{
						OnDone->TriggerFrame(FrameFinished);
					}
				},
				[&](int32 StartFrame, int32 EndFrame)
				{
					UpdateParams();
					EnvelopeState.CurrentSampleIndex = 0;
					EnvelopeState.StartingEnvelopeValue = EnvelopeState.bHardReset ? 0 : EnvelopeState.CurrentEnvelopeValue;
					EnvelopeState.EnvelopeEase.SetValue(EnvelopeState.StartingEnvelopeValue, true);

					TArray<int32> FinishedFrames;
					Envelope::GetNextEnvelopeOutput(EnvelopeState, StartFrame, EndFrame, FinishedFrames, *OutEnvelope);
					for (int32 FrameFinished : FinishedFrames)
					{
						OnDone->TriggerFrame(FrameFinished);
					}
					OnAttackTrigger->TriggerFrame(StartFrame);
				}
			);

			float GateEnvelope = (*OutEnvelope) * ClampedFreq;
			float* OutputAudio = AudioOutput->GetData();
			const float* InputAudio = AudioInput->GetData();

			FAudioBuffer LocalBuffer = FAudioBuffer::FAudioBuffer(AudioInput->Num());
			LocalBuffer.Zero();

			float* LocalInput = LocalBuffer.GetData();

			for (int i = 0; i < AudioInput->Num(); ++i)
			{
				LocalInput[i] = InputAudio[i] * GateEnvelope;
			}

			HandleLowPassFilter();

			VariableFilter.ProcessAudio(LocalInput, AudioInput->Num(), AudioOutput->GetData());
		}
	}

	class FLowPassGateNode : public FNodeFacade
	{
	public:
		FLowPassGateNode(const FNodeInitData& InInitData)
			: FNodeFacade(InInitData.InstanceName, InInitData.InstanceID, TFacadeOperatorClass<FLowPassGateOperator>())
		{}
	};

	METASOUND_REGISTER_NODE(FLowPassGateNode);
}

#undef LOCTEXT_NAMESPACE