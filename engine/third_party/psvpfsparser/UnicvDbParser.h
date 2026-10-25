#pragma once

#include <memory>

#include "UnicvDbTypes.h"

class UnicvDbParser
{
private:
   psvpfs::path m_titleIdPath; // (a copy: the caller's may be a temporary)

   std::unique_ptr<sce_idb_base_t> m_fdb;
   std::ostream& m_output;

public:
   UnicvDbParser(const psvpfs::path& titleIdPath, std::ostream& output);

public:
   int parse();

public:
   const std::unique_ptr<sce_idb_base_t>& get_idatabase() const;
};
