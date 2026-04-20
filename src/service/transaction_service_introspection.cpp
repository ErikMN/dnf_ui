// -----------------------------------------------------------------------------
// transaction_service_introspection.cpp
// Transaction service D-Bus introspection XML
// Keeps object registration data separate from service runtime logic.
// -----------------------------------------------------------------------------
#include "service/transaction_service_introspection.hpp"

const char kTransactionServiceManagerIntrospectionXml[] = R"XML(
<node>
  <interface name="com.fedora.Dnfui.Transaction1">
    <method name="StartTransaction">
      <arg name="install" type="as" direction="in"/>
      <arg name="remove" type="as" direction="in"/>
      <arg name="reinstall" type="as" direction="in"/>
      <arg name="transaction_path" type="o" direction="out"/>
    </method>
  </interface>
</node>
)XML";

const char kTransactionServiceRequestIntrospectionXml[] = R"XML(
<node>
  <interface name="com.fedora.Dnfui.TransactionRequest1">
    <method name="Cancel"/>
    <method name="Apply"/>
    <method name="Release"/>
    <method name="GetPreview">
      <arg name="install" type="as" direction="out"/>
      <arg name="upgrade" type="as" direction="out"/>
      <arg name="downgrade" type="as" direction="out"/>
      <arg name="reinstall" type="as" direction="out"/>
      <arg name="remove" type="as" direction="out"/>
      <arg name="disk_space_delta" type="x" direction="out"/>
    </method>
    <method name="GetResult">
      <arg name="stage" type="s" direction="out"/>
      <arg name="finished" type="b" direction="out"/>
      <arg name="success" type="b" direction="out"/>
      <arg name="details" type="s" direction="out"/>
    </method>
    <signal name="Progress">
      <arg name="line" type="s"/>
    </signal>
    <signal name="Finished">
      <arg name="stage" type="s"/>
      <arg name="success" type="b"/>
      <arg name="details" type="s"/>
    </signal>
  </interface>
</node>
)XML";
