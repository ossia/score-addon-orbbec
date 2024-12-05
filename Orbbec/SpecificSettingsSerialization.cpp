#include "SpecificSettings.hpp"

#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>

template <>
void DataStreamReader::read(const Orbbec::SpecificSettings& n)
{
  m_stream << n.control;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(Orbbec::SpecificSettings& n)
{
  m_stream >> n.control;
  checkDelimiter();
}

template <>
void JSONReader::read(const Orbbec::SpecificSettings& n)
{
  obj["Control"] = n.control;
}

template <>
void JSONWriter::write(Orbbec::SpecificSettings& n)
{
  n.control <<= obj["Control"];
}