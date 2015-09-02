
namespace thor {

// --------------------------------------------------------
// Local APIC management
// --------------------------------------------------------

void initLocalApicOnTheSystem();
void initLocalApicPerCpu();

uint32_t getLocalApicId();

void calibrateApicTimer();

void raiseInitAssertIpi(uint32_t dest_apic_id);

void raiseInitDeassertIpi(uint32_t dest_apic_id);

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page);

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

void setupIoApic(PhysicalAddr address);

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

void setupLegacyPic();

void maskLegacyPic();

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

void acknowledgeIrq(int irq);

} // namespace thor
