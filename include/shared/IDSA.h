#ifndef IDSA_H
#define IDSA_H

#if TS_USE_DSA

#include <cstdlib>
#include <accel-config/libaccel_config.h>

namespace IDSA
{
// Each DSA module has its own instance od Device class.
class Device
{
private:
  static constexpr int MAX_NO_OF_WQ_IN_DEVICE = 16;

  struct accfg_device *d;                  // internal device object
  void *wq_regs[MAX_NO_OF_WQ_IN_DEVICE];   // array of DSA work queues
  int fds[MAX_NO_OF_WQ_IN_DEVICE];         // array of file descriptors
  int wq_idx;                              // will be set to max number of work queues in DSA device
  int numa_node;                           // DSA's numa node
  bool initialize_error;                   // if initialize fails is set to true
  unsigned long task_counter;              // for calculating current work queue
  static const unsigned long dflags;       // descriptor flags (starting with IDXD_ prefix)
  static const unsigned long msec_timeout; // timeout for task in work queue

public:
  Device(struct accfg_device *dev); // takes internal device object
  ~Device();

  bool is_initialize_error();                                  // returns true if something were wrong during initialization
  int get_numa_node();                                         // returns DSA's numa node
  void *memcpy(void *dest, const void *src, std::size_t size); // memmove on DSA (memcpy)
};

// Singleton class which is a container for instances of Device.
// There could be only one instance of this class which can be accessed by
// calling DSA_Devices_Container::getInstance()
// It also contains methods which select Device by numa node.
class DSA_Devices_Container
{
public:
  enum class STATUS { OK, INVALID_NUMA_NODES, INVALID_ACCFG_CTX, MEMCPY_FAILED, ALREADY_INITIALIZED, UNINITIALIZED };

private:
  DSA_Devices_Container(DSA_Devices_Container const &) = delete;
  void operator=(DSA_Devices_Container const &) = delete;

  static constexpr int MAX_NO_OF_DEVICES    = 16;
  static constexpr int MAX_NO_OF_NUMA_NODES = 16;

  DSA_Devices_Container(); // private constructor (singleton)

  Device *devices[MAX_NO_OF_DEVICES];                 // array of devices
  Device *devices_by_numa_node[MAX_NO_OF_NUMA_NODES]; // array of devices which can be accessed by numa node
  int numnodes;                                       // max number of numa nodes
  int dev_idx;                                        // will be set to number of devices
  bool is_initialized;                                // prevents from initialize more than one time
  STATUS current_status;
  STATUS initialize_status = STATUS::UNINITIALIZED;

public:
  static DSA_Devices_Container &getInstance(); // gets instance of singleton
  STATUS initialize();
  void *memcpy_on_DSA(void *dest, const void *src,
                      std::size_t size); // gets dest numa node and perform memcpy on DSA with this numa node
  void *memcpy(void *dest, const void *src,
               std::size_t size); // wrapper (do memcpy on DSA if possible. If not then use std::memcpy)

  ~DSA_Devices_Container(); // calls destructors for each instance of Device
};

} // namespace IDSA
#endif

#endif // IDSA_H