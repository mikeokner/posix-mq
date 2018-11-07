# -*- mode: ruby -*-
# vi: set ft=ruby :

Vagrant.configure("2") do |config|
    config.vm.box = "ubuntu/bionic64"
    config.vm.provision "shell" do |s|
        s.privileged = false
        s.inline = <<-SHELL
            sudo apt-get update && sudo apt-get install -y python build-essential
            curl -o- https://raw.githubusercontent.com/creationix/nvm/v0.33.11/install.sh | bash
            . ~/.nvm/nvm.sh
            nvm install v10
        SHELL
    end
end
