require_relative '../fixtures/classes'

platform_is :windows do
  require 'win32ole'

  describe "WIN32OLE#ole_func_methods" do
    before :each do
      @dict = WIN32OLESpecs.new_ole('Scripting.Dictionary')
    end

    it "raises ArgumentError if argument is given" do
      lambda { @dict.ole_func_methods(1) }.should raise_error ArgumentError
    end

    it "returns an array of WIN32OLE_METHODs" do
      @dict.ole_func_methods.all? { |m| m.kind_of? WIN32OLE_METHOD }.should be_true
    end

    it "contains a 'AddRef' method for Scripting Dictionary" do
      @dict.ole_func_methods.map { |m| m.name }.include?('AddRef').should be_true
    end
  end
end
