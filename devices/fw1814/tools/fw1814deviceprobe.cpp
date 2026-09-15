#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

namespace {
constexpr std::uint32_t kVendor=0x00000d6c;
constexpr std::uint32_t kSpecifier=0x0000a02d;
constexpr std::uint32_t kSpecialFirmware=0x00014001;
struct Unit{std::string product;std::optional<std::uint64_t> guid,vendor,specifier,software;};

std::string stringProperty(io_registry_entry_t s,const char *key){
    CFStringRef k=CFStringCreateWithCString(kCFAllocatorDefault,key,kCFStringEncodingUTF8);if(!k)return {};
    CFTypeRef v=IORegistryEntryCreateCFProperty(s,k,kCFAllocatorDefault,0);CFRelease(k);if(!v)return {};
    std::string result;if(CFGetTypeID(v)==CFStringGetTypeID()){char text[1024]={};
        if(CFStringGetCString(static_cast<CFStringRef>(v),text,sizeof(text),kCFStringEncodingUTF8))result=text;}
    CFRelease(v);return result;
}
std::optional<std::uint64_t> numberProperty(io_registry_entry_t s,const char *key){
    CFStringRef k=CFStringCreateWithCString(kCFAllocatorDefault,key,kCFStringEncodingUTF8);if(!k)return std::nullopt;
    CFTypeRef v=IORegistryEntryCreateCFProperty(s,k,kCFAllocatorDefault,0);CFRelease(k);if(!v)return std::nullopt;
    std::optional<std::uint64_t> result;if(CFGetTypeID(v)==CFNumberGetTypeID()){std::uint64_t n=0;
        if(CFNumberGetValue(static_cast<CFNumberRef>(v),kCFNumberSInt64Type,&n))result=n;}
    CFRelease(v);return result;
}
bool supported(const Unit& u,std::string& personality){
    if(!u.vendor||!u.specifier||!u.software||*u.vendor!=kVendor||*u.specifier!=kSpecifier||*u.software!=kSpecialFirmware)return false;
    if(u.product=="FW 1814"){personality="operational";return true;}
    // Both the generic and model-specific M-Audio bootloader identities are
    // accepted here. fwboot1814 performs the model-specific guarded preflight
    // before sending a boot cue.
    if(u.product=="FW Bootloader"||u.product=="FW 1814 Bootloader"){
        personality="bootloader (guarded by fwboot1814)";return true;
    }
    return false;
}
void printNumber(const char *label,const std::optional<std::uint64_t>& v){
    if(v)std::cout<<"    "<<label<<": 0x"<<std::hex<<*v<<std::dec<<'\n';
}
void usage(const char *p){std::cout<<"Usage: "<<p<<" [--require-supported]\n"
    <<"  --require-supported  exit 3 unless an FW1814 operational or compatible bootloader unit is connected\n";}
}

int main(int argc,char **argv){
    bool require=false;
    if(argc==2&&std::string(argv[1])=="--require-supported")require=true;
    else if(argc==2&&(std::string(argv[1])=="--help"||std::string(argv[1])=="-h")){usage(argv[0]);return 0;}
    else if(argc!=1){usage(argv[0]);return 2;}
    CFMutableDictionaryRef matching=IOServiceMatching("IOFireWireUnit");if(!matching)return 1;
    io_iterator_t it=IO_OBJECT_NULL;if(IOServiceGetMatchingServices(kIOMainPortDefault,matching,&it)!=KERN_SUCCESS)return 1;
    unsigned found=0,accepted=0;io_registry_entry_t service=IO_OBJECT_NULL;
    while((service=IOIteratorNext(it))!=IO_OBJECT_NULL){
        ++found;Unit u{stringProperty(service,"FireWire Product Name"),numberProperty(service,"GUID"),
            numberProperty(service,"Vendor_ID"),numberProperty(service,"Unit_Spec_ID"),numberProperty(service,"Unit_SW_Version")};
        IOObjectRelease(service);std::string personality;bool ok=supported(u,personality);
        std::cout<<"FireWire unit #"<<found<<"\n    product: "<<(u.product.empty()?"<unknown>":u.product)<<'\n';
        printNumber("GUID",u.guid);printNumber("vendor",u.vendor);printNumber("unit spec",u.specifier);printNumber("unit software",u.software);
        std::cout<<"    FW1814 supported: "<<(ok?"yes":"no")<<'\n';if(ok){++accepted;std::cout<<"    personality: "<<personality<<'\n';}
    }
    IOObjectRelease(it);std::cout<<"summary: "<<accepted<<" supported FW1814 unit"<<(accepted==1?"":"s")<<" detected\n";
    if(require&&accepted==0)return 3;return 0;
}
