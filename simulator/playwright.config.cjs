const {defineConfig}=require('@playwright/test');
module.exports=defineConfig({testDir:'./tests',testMatch:'*.spec.cjs',workers:1,use:{baseURL:process.env.GEMMI_TEST_URL||'http://127.0.0.1:3211',viewport:{width:1360,height:900}},reporter:'list'});
