// Exercise the private packetization helpers without adding an application API.
#include "../RtspServer.cpp"
#include <iostream>

namespace
{
void Check(const std::vector<NalUnit>& units, bool idr,
           const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
           const std::vector<uint8_t>& expected, const char* name)
{
    const auto ordered = OrderAccessUnit(units, idr, sps, pps);
    std::vector<uint8_t> types;
    for (const auto& unit : ordered) types.push_back(unit.data[0] & 31);
    if (types != expected) throw std::runtime_error(name);
    // Every original NAL must survive unchanged; cached NALs are optional additions.
    for (const auto& original : units)
        if (std::none_of(ordered.begin(), ordered.end(), [&](const NalUnit& unit) {
                return unit.data == original.data && unit.size == original.size;
            })) throw std::runtime_error("Original NAL lost or replaced");
    std::cout << "PASS: " << name << '\n';
}
}

int main()
{
    try
    {
        const std::vector<uint8_t> sps{0x67,0x42,0,0x28}, pps{0x68,0xce};
        const uint8_t aud[]{0x09,0xf0}, sei[]{0x06,0x80}, idr[]{0x65,0x80}, predicted[]{0x41,0x80};
        const uint8_t otherSps[]{0x67,0x42,0,0x1e};
        const NalUnit a{aud,sizeof(aud)}, s{sps.data(),sps.size()}, p{pps.data(),pps.size()},
                      e{sei,sizeof(sei)}, i{idr,sizeof(idr)}, f{predicted,sizeof(predicted)},
                      s2{otherSps,sizeof(otherSps)};
        Check({a,s,p,i}, true, sps, pps, {9,7,8,5}, "Encoder AUD/SPS/PPS/IDR has no duplicate headers");
        Check({a,e,i}, true, sps, pps, {9,7,8,6,5}, "Cached parameter sets follow AUD and precede SEI/IDR");
        Check({a,p,i}, true, sps, pps, {9,7,8,5}, "Missing SPS is restored before existing PPS");
        Check({a,s,i}, true, sps, pps, {9,7,8,5}, "Missing PPS is restored after existing SPS");
        Check({i}, true, sps, pps, {7,8,5}, "IDR without AUD remains decodable");
        Check({a,s,s2,p,i}, true, sps, pps, {9,7,7,8,5}, "Distinct original SPS NALs are preserved");
        Check({a,e,f}, false, sps, pps, {9,6,1}, "Predicted picture receives no redundant SPS/PPS");
        Check({s,p,a,i}, true, sps, pps, {9,7,8,5}, "Misplaced encoder AUD is normalized");
        Check({a,i}, true, {}, {}, {9,5}, "No fabricated parameter sets when cache is empty");
        const uint8_t annexB[]{0,0,0,1,0x09,0xf0,0,0,1,0x67,0x42,0,0x28,0,0,0,1,0x68,0xce,0,0,1,0x65,0x80};
        const uint8_t avcc[]{0,0,0,2,0x09,0xf0,0,0,0,4,0x67,0x42,0,0x28,0,0,0,2,0x68,0xce,0,0,0,2,0x65,0x80};
        Check(SplitNalUnits(annexB,sizeof(annexB)), true, sps, pps, {9,7,8,5}, "Mixed Annex-B start codes");
        Check(SplitNalUnits(avcc,sizeof(avcc)), true, sps, pps, {9,7,8,5}, "Length-prefixed encoder output");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
