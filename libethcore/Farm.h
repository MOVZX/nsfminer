
#pragma once

#include <atomic>
#include <list>
#include <thread>

#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <boost/dll.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include <json/json.h>

#include <libdevcore/Common.h>
#include <libdevcore/Worker.h>

#include <libethcore/Miner.h>

#include <libhwmon/wrapnvml.h>
#if defined(__linux)
#include <libhwmon/wrapamdsysfs.h>
#include <sys/stat.h>
#else
#include <libhwmon/wrapadl.h>
#endif

using namespace boost::placeholders;

extern boost::asio::io_service g_io_service;

namespace dev
{
namespace eth
{
struct FarmSettings
{
    bool eval = false;         // Whether or not to re-evaluate solutions
    unsigned hwMon = 0;        // 0 - No monitor; 1 - Temp and Fan; 2 - Temp Fan Power
    unsigned tempStart = 40;   // Temperature threshold to restart mining (if paused)
    unsigned tempStop = 0;     // Temperature threshold to pause mining (overheating)
    unsigned cuBlockSize = 0;
    unsigned cuStreams = 0;
    unsigned clGroupSize = 0;
    bool clBin;
};

/**
 * @brief A collective of Miners.
 * Miners ask for work, then submit proofs
 * @threadsafe
 */
class Farm : public FarmFace
{
public:
    unsigned tstart = 0, tstop = 0;

    Farm(std::map<std::string, DeviceDescriptor>& _DevicesCollection, FarmSettings _settings);

    ~Farm();

    static Farm& f() { return *m_this; }

    /**
     * @brief Sets the current mining mission.
     * @param _wp The work package we wish to be mining.
     */
    void setWork(WorkPackage const& _newWp);

    /**
     * @brief Start a number of miners.
     */
    bool start();

    /**
     * @brief All mining activities to a full stop.
     * Implies all mining threads are stopped.
     */
    void stop();

    /**
     * @brief Signals all miners to suspend mining
     */
    void pause();

    /**
     * @brief Whether or not the whole farm has been paused
     */
    bool paused();

    /**
     * @brief Signals all miners to resume mining
     */
    void resume();

    /**
     * @brief Stop all mining activities and Starts them again
     */
    void restart();

    /**
     * @brief Stop all mining activities and Starts them again (async post)
     */
    void restart_async();

    /**
     * @brief Returns whether or not the farm has been started
     */
    bool isMining() const { return m_isMining.load(std::memory_order_relaxed); }

    /**
     * @brief Spawn a reboot script (reboot.bat/reboot.sh)
     * @return false if no matching file was found
     */
    bool reboot(const std::vector<std::string>& args);

    /**
     * @brief Get information on the progress of mining this work package.
     * @return The progress with mining so far.
     */
    TelemetryType& Telemetry() { return m_telemetry; }

    /**
     * @brief Gets current hashrate
     */
    float HashRate() { return m_telemetry.farm.hashrate; };

    /**
     * @brief Gets the collection of pointers to miner instances
     */
    std::vector<std::shared_ptr<Miner>> getMiners() { return m_miners; }

    /**
     * @brief Gets the number of miner instances
     */
    unsigned getMinersCount() { return (unsigned)m_miners.size(); };

    /**
     * @brief Gets the pointer to a miner instance
     */
    std::shared_ptr<Miner> getMiner(unsigned index)
    {
        try
        {
            return m_miners.at(index);
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }

    /**
     * @brief Accounts a solution to a miner and, as a consequence, to
     *  the whole farm
     */
    void accountSolution(unsigned _minerIdx, SolutionAccountingEnum _accounting) override;

    /**
     * @brief Gets the solutions account for the whole farm
     */
    SolutionAccountType getSolutions();

    /**
     * @brief Gets the solutions account for single miner
     */
    SolutionAccountType getSolutions(unsigned _minerIdx);

    using SolutionFound = std::function<void(const Solution&)>;
    using MinerRestart = std::function<void()>;

    /**
     * @brief Provides a valid header based upon that received previously with setWork().
     * @param _bi The now-valid header.
     * @return true if the header was good and that the Farm should pause until more work is
     * submitted.
     */
    void onSolutionFound(SolutionFound const& _handler) { m_onSolutionFound = _handler; }

    void onMinerRestart(MinerRestart const& _handler) { m_onMinerRestart = _handler; }

    /**
     * @brief Provides the description of segments each miner is working on
     * @return a JsonObject
     */

    void setTStartTStop(unsigned tstart, unsigned tstop);

    unsigned get_tstart() override { return m_Settings.tempStart; }

    unsigned get_tstop() override { return m_Settings.tempStop; }

    /**
     * @brief Called from a Miner to note a WorkPackage has a solution.
     * @param _s The solution.
     */
    void submitProof(Solution const& _s) override;

private:
    std::atomic<bool> m_paused = {false};

    // Async submits solution serializing execution
    // in Farm's strand
    void submitProofAsync(Solution const& _s);

    // Collects data about hashing and hardware status
    void collectData(const boost::system::error_code& ec);

    bool spawn_file_in_bin_dir(const char* filename, const std::vector<std::string>& args);

    mutable std::mutex farmWorkMutex;
    std::vector<std::shared_ptr<Miner>> m_miners;  // Collection of miners

    WorkPackage m_currentWp;
    EpochContext m_currentEc;

    std::atomic<bool> m_isMining = {false};

    TelemetryType m_telemetry;  // Holds progress and status info for farm and miners

    SolutionFound m_onSolutionFound;
    MinerRestart m_onMinerRestart;

    FarmSettings m_Settings;  // Own Farm Settings

    boost::asio::io_service::strand m_io_strand;
    boost::asio::deadline_timer m_collectTimer;
    static const int m_collectInterval = 5000;

    // StartNonce (non-NiceHash Mode) and
    // segment width assigned to each GPU as exponent of 2
    // considering an average block time of 15 seconds
    // a single device GPU should need a speed of 286 Mh/s
    // before it consumes the whole 2^32 segment

    // Wrappers for hardware monitoring libraries and their mappers
    wrap_nvml_handle* nvmlh = nullptr;
    std::map<string, int> map_nvml_handle = {};

#if defined(__linux)
    wrap_amdsysfs_handle* sysfsh = nullptr;
    std::map<string, int> map_amdsysfs_handle = {};
#else
    wrap_adl_handle* adlh = nullptr;
    std::map<string, int> map_adl_handle = {};
#endif

    static Farm* m_this;
    std::map<std::string, DeviceDescriptor>& m_DevicesCollection;

    random_device m_engine;
};

}  // namespace eth
}  // namespace dev
