
#include "cucumber_cpp/library/engine/TestRunner.hpp"
#include "cucumber_cpp/library/HookRegistry.hpp"
#include "cucumber_cpp/library/OnTestPartResultEventListener.hpp"
#include "cucumber_cpp/library/Rtrim.hpp"
#include "cucumber_cpp/library/StepRegistry.hpp"
#include "cucumber_cpp/library/engine/ContextManager.hpp"
#include "cucumber_cpp/library/engine/FeatureInfo.hpp"
#include "cucumber_cpp/library/engine/Result.hpp"
#include "cucumber_cpp/library/engine/RuleInfo.hpp"
#include "cucumber_cpp/library/engine/StepInfo.hpp"
#include "cucumber_cpp/library/report/Report.hpp"
#include "gtest/gtest.h"
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace cucumber_cpp::engine
{
    void RunTestPolicy::ExecuteStep(ContextManager& contextManager, const StepInfo& stepInfo, const StepMatch& stepMatch) const
    {
        stepMatch.factory(contextManager.CurrentContext(), stepInfo.Table())->Execute(stepMatch.matches);
    }

    bool RunTestPolicy::ExecuteHook(ContextManager& context, HookType hook, const std::set<std::string, std::less<>>& tags) const
    {
        try
        {
            for (const auto& match : HookRegistry::Instance().Query(hook, tags))
                match.factory(context.CurrentContext())->Execute();
        }
        catch (std::exception& /* ex */)
        {
            return false;
        }
        catch (...)
        {
            return false;
        }

        return true;
    }

    void DryRunPolicy::ExecuteStep(ContextManager& /* contextManager */, const StepInfo& /* stepInfo */, const StepMatch& /* stepMatch */) const
    {
        /* dry run doesn't execute */
    }

    bool DryRunPolicy::ExecuteHook(ContextManager& /* context */, HookType /* hook */, const std::set<std::string, std::less<>>& /* tags */) const
    {
        return true;
    }

    namespace TestRunner
    {
        namespace
        {
            struct AppendFailureOnTestPartResultEvent
                : OnTestPartResultEventListener
            {
                explicit AppendFailureOnTestPartResultEvent(report::ReportHandlerV2& reportHandler)
                    : reportHandler{ reportHandler }
                {
                }

                ~AppendFailureOnTestPartResultEvent() override
                {
                    for (const auto& error : errors)
                        reportHandler.Failure(error.message(), error.file_name(), error.line_number(), 0);
                }

                AppendFailureOnTestPartResultEvent(const AppendFailureOnTestPartResultEvent&) = delete;
                AppendFailureOnTestPartResultEvent& operator=(const AppendFailureOnTestPartResultEvent&) = delete;

                AppendFailureOnTestPartResultEvent(AppendFailureOnTestPartResultEvent&&) = delete;
                AppendFailureOnTestPartResultEvent& operator=(AppendFailureOnTestPartResultEvent&&) = delete;

                void OnTestPartResult(const testing::TestPartResult& testPartResult) override
                {
                    errors.emplace_back(testPartResult);
                }

                [[nodiscard]] bool HasFailures() const
                {
                    return !errors.empty();
                }

            private:
                report::ReportHandlerV2& reportHandler;
                std::vector<testing::TestPartResult> errors;
            };

            struct CaptureAndTraceStdOut
            {
                explicit CaptureAndTraceStdOut(report::ReportHandlerV2& reportHandler)
                    : reportHandler{ reportHandler }
                {
                    testing::internal::CaptureStdout();
                    testing::internal::CaptureStderr();
                }

                ~CaptureAndTraceStdOut()
                {
                    if (auto str = testing::internal::GetCapturedStdout(); !str.empty())
                    {
                        Rtrim(str);
                        reportHandler.Trace(str);
                    }

                    if (auto str = testing::internal::GetCapturedStderr(); !str.empty())
                    {
                        Rtrim(str);
                        reportHandler.Trace(str);
                    }
                }

                CaptureAndTraceStdOut(const CaptureAndTraceStdOut&) = delete;
                CaptureAndTraceStdOut& operator=(const CaptureAndTraceStdOut&) = delete;

                CaptureAndTraceStdOut(CaptureAndTraceStdOut&&) = delete;
                CaptureAndTraceStdOut& operator=(CaptureAndTraceStdOut&&) = delete;

            private:
                report::ReportHandlerV2& reportHandler;
            };

            struct ExecuteHookOnDescruction
            {
                ExecuteHookOnDescruction(ContextManager& context, HookType hook, const std::set<std::string, std::less<>>& tags, const RunPolicy& runPolicy)
                    : context{ context }
                    , hook{ hook }
                    , tags{ tags }
                    , runPolicy{ runPolicy }
                {}

                ~ExecuteHookOnDescruction()
                {
                    runPolicy.ExecuteHook(context, hook, tags);
                }

                ExecuteHookOnDescruction(const ExecuteHookOnDescruction&) = delete;
                ExecuteHookOnDescruction& operator=(const ExecuteHookOnDescruction&) = delete;
                ExecuteHookOnDescruction(ExecuteHookOnDescruction&&) = delete;
                ExecuteHookOnDescruction& operator=(ExecuteHookOnDescruction&&) = delete;

            private:
                ContextManager& context;
                HookType hook;
                const std::set<std::string, std::less<>>& tags;
                const RunPolicy& runPolicy;
            };

            [[noreturn]] void ExecuteStep(const ContextManager& /* contextManager */, const ScenarioInfo& /*scenario*/, const StepInfo& /* stepInfo */, std::monostate /* */, const RunPolicy& /* runPolicy */)
            {
                throw StepNotFoundException{};
            }

            [[noreturn]] void ExecuteStep(const ContextManager& /* contextManager */, const ScenarioInfo& /*scenario*/, const StepInfo& /* stepInfo */, const std::vector<StepMatch>& /* */, const RunPolicy& /* runPolicy */)
            {
                throw AmbiguousStepException{};
            }

            void ExecuteStep(ContextManager& contextManager, const ScenarioInfo& scenario, const StepInfo& stepInfo, const StepMatch& stepMatch, const RunPolicy& runPolicy)
            {
                ExecuteHookOnDescruction afterStepHook{ contextManager, HookType::afterStep, scenario.Tags(), runPolicy };

                if (runPolicy.ExecuteHook(contextManager, HookType::beforeStep, scenario.Tags()))
                    runPolicy.ExecuteStep(contextManager, stepInfo, stepMatch);
            }

            void StepResult(ContextManager& contextManager, Result result)
            {
                contextManager.StepContext().ExecutionStatus(result);
            }

            void ExecuteStep(ContextManager& contextManager, const ScenarioInfo& scenario, const StepInfo& stepInfo, const RunPolicy& runPolicy)
            {
                try
                {
                    std::visit([&contextManager, &stepInfo, &runPolicy, &scenario](auto& value)
                        {
                            ExecuteStep(contextManager, scenario, stepInfo, value, runPolicy);
                        },
                        stepInfo.StepMatch());
                }
                catch (PendingException& /* ex */)
                {
                    StepResult(contextManager, Result::pending);
                }
                catch (StepNotFoundException& /* ex */)
                {
                    StepResult(contextManager, Result::undefined);
                }
                catch (AmbiguousStepException& /* ex */)
                {
                    StepResult(contextManager, Result::ambiguous);
                }
                catch (std::exception& /* ex */) // NOLINT
                {
                    StepResult(contextManager, Result::failed);
                }
                catch (...)
                {
                    StepResult(contextManager, Result::failed);
                }
            }
        }

        void WrapExecuteStep(ContextManager& contextManager, const ScenarioInfo& scenario, const StepInfo& step, report::ReportHandlerV2& reportHandler, const RunPolicy& runPolicy)
        {
            AppendFailureOnTestPartResultEvent appendFailureOnTestPartResultEvent{ reportHandler };
            CaptureAndTraceStdOut captureAndTraceStdOut{ reportHandler };

            ExecuteStep(contextManager, scenario, step, runPolicy);

            if (appendFailureOnTestPartResultEvent.HasFailures())
                StepResult(contextManager, engine::Result::failed);
        }

        void ManageExecuteStep(ContextManager& contextManager, const ScenarioInfo& scenario, const StepInfo& step, report::ReportHandlerV2& reportHandler, const RunPolicy& runPolicy)
        {
            ContextManager::ScopedContextLock contextScope{ contextManager.StartScope(step) };

            if (const auto mustSkip = contextManager.CurrentContext().ExecutionStatus() != Result::passed)
                reportHandler.StepSkipped(step);
            else
            {
                reportHandler.StepStart(step);

                WrapExecuteStep(contextManager, scenario, step, reportHandler, runPolicy);

                reportHandler.StepEnd(contextManager.StepContext().ExecutionStatus(), step, contextManager.StepContext().Duration());
            }
        }

        void ExecuteSteps(ContextManager& contextManager, const ScenarioInfo& scenario, report::ReportHandlerV2& reportHandler, const RunPolicy& runPolicy)
        {
            for (const auto& step : scenario.Children())
            {
                ManageExecuteStep(contextManager, scenario, *step, reportHandler, runPolicy);
            }
        }

        void RunScenarios(ContextManager& contextManager, const std::vector<std::unique_ptr<ScenarioInfo>>& scenarios, report::ReportHandlerV2& reportHandler, const RunPolicy& runPolicy)
        {
            bool allScenariosPassed = true;
            for (const auto& scenario : scenarios)
            {
                ContextManager::ScopedContextLock contextScope{ contextManager.StartScope(*scenario) };

                reportHandler.ScenarioStart(*scenario);

                if (runPolicy.ExecuteHook(contextManager, HookType::before, scenario->Tags()))
                    ExecuteSteps(contextManager, *scenario, reportHandler, runPolicy);

                runPolicy.ExecuteHook(contextManager, HookType::after, scenario->Tags());

                reportHandler.ScenarioEnd(contextManager.CurrentContext().ExecutionStatus(), *scenario, contextManager.CurrentContext().Duration());
                allScenariosPassed &= contextManager.CurrentContext().ExecutionStatus() == Result::passed;
            }
            contextManager.CurrentContext().ExecutionStatus(allScenariosPassed ? Result::passed : Result::failed);
        }

        void RunRules(ContextManager& contextManager, const std::vector<std::unique_ptr<RuleInfo>>& rules, report::ReportHandlerV2& reportHandler, const RunPolicy& runPolicy)
        {
            for (const auto& rule : rules)
            {
                ContextManager::ScopedContextLock contextScope{ contextManager.StartScope(*rule) };

                reportHandler.RuleStart(*rule);

                RunScenarios(contextManager, rule->Scenarios(), reportHandler, runPolicy);

                reportHandler.RuleEnd(contextManager.CurrentContext().ExecutionStatus(), *rule, contextManager.CurrentContext().Duration());
            }
        }

        void RunFeature(ContextManager& contextManager, const FeatureInfo& feature, report::ReportHandlerV2& reportHandler, const RunPolicy& runPolicy)
        {
            if (feature.Rules().empty() && feature.Scenarios().empty())
                return;

            ContextManager::ScopedContextLock contextScope{ contextManager.StartScope(feature) };

            reportHandler.FeatureStart(feature);

            if (runPolicy.ExecuteHook(contextManager, HookType::beforeFeature, feature.Tags()))
            {
                RunRules(contextManager, feature.Rules(), reportHandler, runPolicy);
                RunScenarios(contextManager, feature.Scenarios(), reportHandler, runPolicy);
            }

            runPolicy.ExecuteHook(contextManager, HookType::afterFeature, feature.Tags());

            reportHandler.FeatureEnd(contextManager.CurrentContext().ExecutionStatus(), feature, contextManager.CurrentContext().Duration());
        }

        void Run(ContextManager& contextManager, const std::vector<std::unique_ptr<FeatureInfo>>& feature, report::ReportHandlerV2& reportHandler, const RunPolicy& runPolicy)
        {
            if (runPolicy.ExecuteHook(contextManager, HookType::beforeAll, {}))
                for (const auto& featurePtr : feature)
                    RunFeature(contextManager, *featurePtr, reportHandler, runPolicy);

            runPolicy.ExecuteHook(contextManager, HookType::afterAll, {});

            reportHandler.Summary(contextManager.CurrentContext().Duration());
        }
    }
}
