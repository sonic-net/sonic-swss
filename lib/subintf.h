#pragma once

#include <string>

#define VLAN_SUB_INTERFACE_SEPARATOR   "."
namespace swss {
	class subIntf
	{
		public:
			subIntf(const std::string &ifName);
			bool isValid() const;
			std::string parentIntf() const;
			int subIntfIdx() const;
			std::string longName() const;
			std::string shortName() const;
			bool isShortName() const;

        private:
			std::string alias;
			std::string subIfIdx;
			std::string parentIf;
			std::string parentIfShortName;
			bool isCompressed = false;
	};
}
