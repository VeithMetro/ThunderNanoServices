#include "Module.h"
#include <bitset>
#include <core/ProcessInfo.h>
#include <fstream>
#include <interfaces/IMemory.h>
#include <interfaces/IResourceMonitor.h>
#include <numeric>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace WPEFramework {
namespace Plugin {
    class ResourceMonitorImplementation : public Exchange::IResourceMonitor {
    private:
        class CSVFile {
        public:
            CSVFile(string filepath, string seperator)
                : _file(filepath)
                , _seperator()
            {
                _file.Create();
                if (!_file.IsOpen()) {
                    TRACE(Trace::Error, (_T("Could not open file <%s>. Full resource monitoring unavailable."), filepath));
                } else {
                    if (seperator.empty() || seperator.size() > 1) {
                        TRACE(Trace::Error, (_T("Invalid seperator, falling back to ';'.")));
                        _seperator = ';';
                    } else {
                        _seperator = seperator[0];
                    }
                }
                _stringstream.precision(2);
            }

            ~CSVFile()
            {
                if (_file.IsOpen()) {
                    _file.Close();
                }
            }

            void Store()
            {
                if (_file.IsOpen()) {
                    std::string output = _stringstream.str();
                    _file.Write(reinterpret_cast<const uint8_t*>(output.c_str()), output.length());
                }
                _stringstream.str("");
            }

            template <typename FIRST, typename... Args>
            void Append(const FIRST& first, Args... rest)
            {
                _stringstream << first;
                Append(true, rest...);
            }

        private:
            template <typename FIRST, typename... Args>
            void Append(bool dummy, const FIRST& first, Args... rest)
            {
                _stringstream << _seperator << first;
                Append(dummy, rest...);
            }

            void Append(bool dummy)
            {
                _stringstream << std::endl;
            }

            Core::File _file;
            std::stringstream _stringstream;
            char _seperator;
        };

        class Config : public Core::JSON::Container {
        public:
            Config& operator=(const Config&) = delete;

            Config()
                : Core::JSON::Container()
                , Path(_T("/tmp/resource.csv"))
                , Seperator(_T(";"))
                , Interval(5)
            {
                Add(_T("csv_filepath"), &Path);
                Add(_T("csv_sep"), &Seperator);
                Add(_T("interval"), &Interval);
                Add(_T("names"), &FilterNames);
            }

            Config(const Config& copy)
                : Core::JSON::Container()
                , Path(copy.Path)
                , Interval(copy.Interval)
                , FilterNames(copy.FilterNames)
            {
            }

            ~Config()
            {
            }

        public:
            Core::JSON::String Path;
            Core::JSON::String Seperator;
            Core::JSON::DecUInt32 Interval;
            Core::JSON::ArrayType<Core::JSON::String> FilterNames;
        };

        class StatCollecter {
        private:
            class Worker : public Core::IDispatch {
            public:
                Worker(StatCollecter* parent)
                    : _parent(*parent)
                {
                }

                void Dispatch() override
                {
                    _parent._guard.Lock();
                    _parent.Dispatch();
                    _parent._guard.Unlock();

                    Core::IWorkerPool::Instance().Schedule(Core::Time::Now().Add(_parent._interval * 1000), Core::ProxyType<Core::IDispatch>(*this));
                }

            private:
                StatCollecter& _parent;
            };

        public:
            explicit StatCollecter(const Config& config)
                : _userCpuTime(0)
                , _systemCpuTime(0)
                , _logfile(config.Path.Value(), config.Seperator.Value())
                , _interval(config.Interval.Value())
                , _worker(Core::ProxyType<Worker>::Create(this))

            {
                _logfile.Append("Time[s]", "Name", "USS[KiB]", "PSS[KiB]", "RSS[KiB]", "VSS[KiB]", "UserTotalCPU[%]", "SystemTotalCPU[%]");

                if (config.FilterNames.Elements().Count() == 0) {
                    _filterNames.push_back("WPE");
                } else {
                    auto filterIterator(config.FilterNames.Elements());
                    while (filterIterator.Next()) {
                        _filterNames.push_back(filterIterator.Current().Value());
                    }
                }

                Core::IWorkerPool::Instance().Schedule(Core::Time::Now(), Core::ProxyType<Core::IDispatch>(_worker));
            }

            ~StatCollecter()
            {
                Core::IWorkerPool::Instance().Revoke(Core::ProxyType<Core::IDispatch>(_worker), Core::infinite);
            }

        private:
            void GetTotalTime(Core::process_t pid)
            {
                std::ifstream stat("/proc/stat");
                if (!stat.is_open()) {
                    TRACE(Trace::Error, (_T("Could not open /proc/stat.")));

                    return;
                }
                std::string line;
                std::istringstream iss;

                std::getline(stat, line);
                iss.str(line);

                std::string dummy;
                uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0, guest = 0;

                iss >> dummy >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest;
                _processTimeInfo[pid].totalTime = user + nice + system + idle + iowait + irq + softirq + steal + guest;
            }

            void GetProcessTimes(Core::process_t pid)
            {
                std::ostringstream pathToSmaps;
                pathToSmaps << "/proc/" << pid << "/stat";

                std::ifstream stat(pathToSmaps.str());
                if (!stat.is_open()) {
                    TRACE(Trace::Error, (_T("Could not open %s"), pathToSmaps.str()));
                    return;
                }

                std::string uTimeString, sTimeString;

                //ignoring 13 words spearated by space
                for (uint32_t i = 0; i < 13; ++i) {
                    stat.ignore(std::numeric_limits<std::streamsize>::max(), ' ');
                }

                stat >> _processTimeInfo[pid].uTime >> _processTimeInfo[pid].sTime;
            }

            void CalculateCpuUsage(Core::process_t pid)
            {
                GetTotalTime(pid);
                GetProcessTimes(pid);

                _userCpuTime = 100.0 * (_processTimeInfo[pid].uTime - _processTimeInfo[pid].prevUTime) / (_processTimeInfo[pid].totalTime - _processTimeInfo[pid].prevTotalTime);

                _systemCpuTime = 100.0 * (_processTimeInfo[pid].sTime - _processTimeInfo[pid].prevSTime) / (_processTimeInfo[pid].totalTime - _processTimeInfo[pid].prevTotalTime);

                _processTimeInfo[pid].prevTotalTime = _processTimeInfo[pid].totalTime;
                _processTimeInfo[pid].prevSTime = _processTimeInfo[pid].sTime;
                _processTimeInfo[pid].prevUTime = _processTimeInfo[pid].uTime;
            }

            void Dispatch()
            {

                for (const auto& filterName : _filterNames) {
                    std::list<Core::ProcessInfo> processes;
                    Core::ProcessInfo::FindByName(filterName, false, processes);

                    for (const Core::ProcessInfo& process : processes) {
                        CalculateCpuUsage(process.Id());
                        LogProcess(process);
                    }
                }
            }

            void LogProcess(const Core::ProcessInfo& process)
            {
                auto timestamp = static_cast<uint32_t>(Core::Time::Now().Ticks() / 1000 / 1000);
                string name = process.Name() + " (" + std::to_string(process.Id()) + ")";
                process.MemoryStats();

                _logfile.Append(timestamp, name, process.USS(), process.PSS(), process.RSS(), process.VSS(), _userCpuTime, _systemCpuTime);
                _logfile.Store();
            }

        private:
            struct Time {
                Time()
                    : totalTime(0)
                    , sTime(0)
                    , uTime(0)
                    , prevTotalTime(0)
                    , prevUTime(0)
                    , prevSTime(0)
                {
                }
                uint64_t totalTime;
                uint64_t sTime;
                uint64_t uTime;
                uint64_t prevTotalTime;
                uint64_t prevUTime;
                uint64_t prevSTime;
            };

            //NOTE: these two indicate usage of the whole CPU, not the single core (as 'top' shows by default)
            double _userCpuTime;
            double _systemCpuTime;

            std::map<Core::process_t, Time> _processTimeInfo;

            CSVFile _logfile;
            uint32_t _interval;
            std::list<std::string> _filterNames;

            Core::CriticalSection _guard;
            Core::ProxyType<Worker> _worker;
        };

    private:
        ResourceMonitorImplementation(const ResourceMonitorImplementation&) = delete;
        ResourceMonitorImplementation& operator=(const ResourceMonitorImplementation&) = delete;

    public:
        ResourceMonitorImplementation()
            : _processThread(nullptr)
            , _csvFilePath()
        {
        }

        ~ResourceMonitorImplementation() override = default;

        virtual uint32_t Configure(PluginHost::IShell* service) override
        {
            uint32_t result(Core::ERROR_INCOMPLETE_CONFIG);

            ASSERT(service != nullptr);

            Config config;
            config.FromString(service->ConfigLine());

            if (config.Interval.Value() <= 0) {
                TRACE(Trace::Error, (_T("Interval must be greater than 0!")));
            } else {
                _processThread.reset(new StatCollecter(config));
                result = Core::ERROR_NONE;
            }

            return (result);
        }

        string CompileMemoryCsv() override
        {
            const uint8_t lastMeasurementsHistory = 10;

            std::ifstream csvFile(_csvFilePath, std::fstream::binary);
            std::ostringstream result;
            std::string line;
            std::vector<std::string> allLines;

            while (std::getline(csvFile, line)) {
                allLines.push_back(line);
            }
            if (allLines.size() > lastMeasurementsHistory) {

                result << allLines.front() << "\n"; //write header

                for (auto currentLine = allLines.end() - lastMeasurementsHistory; currentLine != allLines.end(); ++currentLine) {
                    result << *currentLine << "\n";
                }
            } else {
                result << "Not enough measurements yet!" << std::endl;
            }

            result.flush();
            return result.str();
        }

        BEGIN_INTERFACE_MAP(ResourceMonitorImplementation)
        INTERFACE_ENTRY(Exchange::IResourceMonitor)
        END_INTERFACE_MAP

    private:
        std::unique_ptr<StatCollecter> _processThread;
        string _csvFilePath;
    };

    SERVICE_REGISTRATION(ResourceMonitorImplementation, 1, 0);
} /* namespace Plugin */
} // namespace WPEFramework
